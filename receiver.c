/* receiver.c — Single-threaded jitter buffer + FEC recovery + NACK receiver
 *
 * Ports (all 127.0.0.1):
 *   bind 47002  <- media from sender, via the hostile relay
 *   send 47020  -> harness player (4B BE seq + 160B payload = 164B)
 *   send 47003  -> feedback NACKs to sender, via the relay
 *
 * Wire format (from sender):
 *   DATA (165B):       [0x01][seq BE 4B][payload 160B]
 *   FEC_PARITY (166B): [0x02][group_start BE 4B][parity 160B][group_size 1B]
 *
 * Strategy:
 *   - Single-threaded event loop (no pthreads, no mutex, no clock_nanosleep)
 *   - Jitter buffer with seq-tagged circular slots
 *   - Separate FEC parity cache for XOR recovery (FEC_N=2)
 *   - NACK generation with timing guards
 *   - Playout driven by time checks in the main loop
 *
 * Env vars: T0, DURATION_S, DELAY_MS
 */
#include <arpa/inet.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define PAYLOAD_LEN         160
#define FEC_N               2
#define MAX_BUF             2048
#define MAX_FEC             1024
#define MAX_NACK_PER_FRAME  3
#define MAX_NACKS_PER_SCAN  5

#define PKT_DATA            0x01
#define PKT_FEC             0x02
#define PKT_NACK            0x03

/* ---- Data structures ---- */

struct slot {
    uint32_t seq_tag;
    uint8_t  payload[PAYLOAD_LEN];
    int      received;
};

struct fec_entry {
    uint32_t group_start;
    uint8_t  parity[PAYLOAD_LEN];
    uint8_t  group_size;
    int      valid;
};

/* ---- Globals ---- */

static struct slot buffer[MAX_BUF];
static struct fec_entry fec_cache[MAX_FEC];
static uint8_t nack_count[MAX_BUF];

static double get_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Try FEC recovery for a group. */
static void try_fec_recovery(uint32_t group_start, uint8_t group_size,
                             const uint8_t *parity, int next_playout_seq) {
    int missing_count = 0;
    uint32_t missing_seq = 0;

    for (uint32_t k = 0; k < group_size; k++) {
        uint32_t s = group_start + k;
        struct slot *sl = &buffer[s % MAX_BUF];
        if (!(sl->seq_tag == s && sl->received)) {
            missing_count++;
            missing_seq = s;
        }
    }

    if (missing_count != 1) return;

    /* Check expiry and window */
    if ((int)missing_seq < next_playout_seq) return;
    if ((int)missing_seq >= next_playout_seq + MAX_BUF) return;

    /* Recover by XOR: missing = parity XOR all_present */
    uint8_t recovered[PAYLOAD_LEN];
    memcpy(recovered, parity, PAYLOAD_LEN);
    for (uint32_t k = 0; k < group_size; k++) {
        uint32_t s = group_start + k;
        if (s == missing_seq) continue;
        struct slot *sl = &buffer[s % MAX_BUF];
        for (int j = 0; j < PAYLOAD_LEN; j++) {
            recovered[j] ^= sl->payload[j];
        }
    }

    /* Store recovered frame */
    struct slot *ms = &buffer[missing_seq % MAX_BUF];
    ms->seq_tag = missing_seq;
    memcpy(ms->payload, recovered, PAYLOAD_LEN);
    ms->received = 1;
}

int main(void) {
    /* Parse env vars */
    const char *t0_s = getenv("T0");
    const char *dur_s = getenv("DURATION_S");
    const char *del_s = getenv("DELAY_MS");
    double t0 = t0_s ? atof(t0_s) : 0.0;
    double duration = dur_s ? atof(dur_s) : 30.0;
    double delay_ms = del_s ? atof(del_s) : 60.0;
    int n_frames = (int)(duration * 1000) / 20;
    double delay_s = delay_ms / 1000.0;

    int DOWN_BUDGET = (int)(0.2 * n_frames * PAYLOAD_LEN);
    int down_bytes_sent = 0;

    /* Initialize buffers */
    memset(buffer, 0, sizeof(buffer));
    memset(fec_cache, 0, sizeof(fec_cache));
    memset(nack_count, 0, sizeof(nack_count));

    /* Socket: receive from relay (port 47002) */
    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in in_addr = {0};
    in_addr.sin_family = AF_INET;
    in_addr.sin_port = htons(47002);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(in_fd, (struct sockaddr *)&in_addr, sizeof(in_addr)) < 0) {
        perror("bind 47002");
        return 1;
    }
    fcntl(in_fd, F_SETFL, O_NONBLOCK);

    /* Socket: send to harness player (port 47020) */
    int player_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in player_addr = {0};
    player_addr.sin_family = AF_INET;
    player_addr.sin_port = htons(47020);
    player_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    /* Socket: send NACKs to relay (port 47003) */
    int nack_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in nack_dest = {0};
    nack_dest.sin_family = AF_INET;
    nack_dest.sin_port = htons(47003);
    nack_dest.sin_addr.s_addr = inet_addr("127.0.0.1");

    /* NACK timing parameters */
    double nack_guard = fmax(20.0, delay_ms * 0.25) / 1000.0;
    double nack_grace = (delay_ms * 0.5) / 1000.0;
    int lookahead = (int)(delay_ms / 20);
    if (lookahead < 10) lookahead = 10;
    if (lookahead > MAX_BUF) lookahead = MAX_BUF;

    int next_playout_seq = 0;
    double last_nack_scan = 0.0;
    unsigned char buf[2048];

    /* ---- Single-threaded event loop ---- */
    while (next_playout_seq < n_frames) {
        double now = get_time();

        /* ===== PLAYOUT: send any due frames ===== */
        while (next_playout_seq < n_frames) {
            double deadline = t0 + delay_s + next_playout_seq * 0.020;
            /* Send 2ms before deadline to give UDP loopback + player time */
            if (now < deadline - 0.002) break;

            struct slot *s = &buffer[next_playout_seq % MAX_BUF];
            if (s->seq_tag == (uint32_t)next_playout_seq && s->received) {
                uint8_t out[4 + PAYLOAD_LEN];
                uint32_t net_seq = htonl((uint32_t)next_playout_seq);
                memcpy(out, &net_seq, 4);
                memcpy(out + 4, s->payload, PAYLOAD_LEN);
                sendto(player_fd, out, sizeof(out), 0,
                       (struct sockaddr *)&player_addr, sizeof(player_addr));
            }
            /* Clear slot regardless */
            s->received = 0;
            nack_count[next_playout_seq % MAX_BUF] = 0;
            next_playout_seq++;
        }

        if (next_playout_seq >= n_frames) break;

        /* ===== RECEIVE: non-blocking packet read ===== */
        /* Calculate how long we can afford to block in select.
         * We must wake up before the next playout deadline. */
        double next_deadline = t0 + delay_s + next_playout_seq * 0.020 - 0.002;
        double wait_s = next_deadline - get_time();
        if (wait_s < 0.0) wait_s = 0.0;
        if (wait_s > 0.005) wait_s = 0.005; /* max 5ms */

        long wait_us = (long)(wait_s * 1e6);
        if (wait_us < 0) wait_us = 0;

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(in_fd, &rfds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = wait_us;
        int ready = select(in_fd + 1, &rfds, NULL, NULL, &tv);

        if (ready > 0 && FD_ISSET(in_fd, &rfds)) {
            for (;;) {
                ssize_t n = recvfrom(in_fd, buf, sizeof(buf), 0, NULL, NULL);
                if (n < 0) break;

                if (n == 1 + 4 + PAYLOAD_LEN && buf[0] == PKT_DATA) {
                    /* DATA packet */
                    uint32_t seq;
                    memcpy(&seq, buf + 1, 4);
                    seq = ntohl(seq);

                    /* Window check */
                    if ((int)seq >= next_playout_seq &&
                        (int)seq < next_playout_seq + MAX_BUF &&
                        (int)seq < n_frames) {
                        struct slot *sl = &buffer[seq % MAX_BUF];

                        /* Duplicate check */
                        if (!(sl->seq_tag == seq && sl->received)) {
                            sl->seq_tag = seq;
                            memcpy(sl->payload, buf + 5, PAYLOAD_LEN);
                            sl->received = 1;

                            /* Try FEC recovery for this frame's group */
                            uint32_t gs = (seq / FEC_N) * FEC_N;
                            int fec_idx = (gs / FEC_N) % MAX_FEC;
                            struct fec_entry *fe = &fec_cache[fec_idx];
                            if (fe->valid && fe->group_start == gs) {
                                try_fec_recovery(gs, fe->group_size,
                                                 fe->parity, next_playout_seq);
                            }
                        }
                    }

                } else if (n == 1 + 4 + PAYLOAD_LEN + 1 && buf[0] == PKT_FEC) {
                    /* FEC PARITY packet */
                    uint32_t group_start;
                    memcpy(&group_start, buf + 1, 4);
                    group_start = ntohl(group_start);
                    uint8_t group_size = buf[5 + PAYLOAD_LEN];

                    if (group_size >= 1 && group_size <= FEC_N) {
                        /* Window check: is entire group expired? */
                        if ((int)(group_start + group_size - 1) >= next_playout_seq) {
                            int fec_idx = (group_start / FEC_N) % MAX_FEC;
                            struct fec_entry *fe = &fec_cache[fec_idx];
                            fe->group_start = group_start;
                            memcpy(fe->parity, buf + 5, PAYLOAD_LEN);
                            fe->group_size = group_size;
                            fe->valid = 1;

                            /* Immediate recovery attempt */
                            try_fec_recovery(group_start, group_size,
                                             fe->parity, next_playout_seq);
                        }
                    }
                }
            }
        }

        /* ===== NACK generation — every 10ms ===== */
        now = get_time();
        if (now - last_nack_scan >= 0.010) {
            last_nack_scan = now;
            int nacks_sent = 0;

            int scan_end = next_playout_seq + lookahead;
            if (scan_end > n_frames) scan_end = n_frames;

            for (int i = next_playout_seq;
                 i < scan_end && nacks_sent < MAX_NACKS_PER_SCAN; i++) {
                /* Rule 1: Don't NACK frames before they've had time to arrive */
                double source_send_time = t0 + i * 0.020;
                if (now < source_send_time + nack_grace) continue;

                /* Rule 2: Frame must not be received already */
                struct slot *s = &buffer[i % MAX_BUF];
                if (s->seq_tag == (uint32_t)i && s->received) continue;

                /* Rule 3: Must have enough time for NACK round trip */
                double deadline_time = t0 + delay_s + i * 0.020;
                double remaining = deadline_time - now;
                if (remaining <= nack_guard) continue;

                /* Rule 4: Don't over-NACK */
                if (nack_count[i % MAX_BUF] >= MAX_NACK_PER_FRAME) continue;

                /* Rule 5: Budget check */
                if (down_bytes_sent + 5 > DOWN_BUDGET) break;

                /* Send NACK */
                uint8_t nack_pkt[5];
                nack_pkt[0] = PKT_NACK;
                uint32_t net_seq = htonl((uint32_t)i);
                memcpy(nack_pkt + 1, &net_seq, 4);
                sendto(nack_fd, nack_pkt, 5, 0,
                       (struct sockaddr *)&nack_dest, sizeof(nack_dest));
                nack_count[i % MAX_BUF]++;
                down_bytes_sent += 5;
                nacks_sent++;
            }
        }
    }

    return 0;
}
