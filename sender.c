/* sender.c — FEC + NACK-based retransmission sender
 *
 * Ports (all 127.0.0.1):
 *   bind 47010  <- harness source delivers frame i at t0 + i*20ms
 *                  (format: 4-byte big-endian seq + 160-byte payload)
 *   send 47001  -> relay uplink (custom wire format)
 *   bind 47004  <- feedback NACKs from receiver, via relay
 *
 * Wire format:
 *   DATA (165B):       [0x01][seq BE 4B][payload 160B]
 *   FEC_PARITY (166B): [0x02][group_start BE 4B][parity 160B][group_size 1B]
 *   NACK (5B):         [0x03][seq BE 4B]
 *
 * Strategy:
 *   - Forward each frame as a DATA packet
 *   - Every FEC_N=2 frames, send XOR parity for the group
 *   - Listen for NACKs and retransmit from circular buffer (max 2 retx/frame)
 *   - Enforce uplink budget of 1.8x raw stream bytes
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
#include <unistd.h>

#define PAYLOAD_LEN     160
#define FEC_N           2
#define RETX_BUF_SIZE   512
#define MAX_RETX        2

#define PKT_DATA        0x01
#define PKT_FEC         0x02
#define PKT_NACK        0x03

struct retx_slot {
    uint32_t seq_tag;
    uint8_t  payload[PAYLOAD_LEN];
    int      retransmit_count;
    int      valid;
};

static struct retx_slot retx_buf[RETX_BUF_SIZE];
static uint8_t fec_parity[PAYLOAD_LEN];
static int fec_count = 0;
static uint32_t fec_group_start = 0;

static int up_bytes_sent = 0;
static int UP_BUDGET = 0;

/* Send a packet to the relay, respecting the budget */
static int budget_send(int fd, const void *buf, size_t len,
                       const struct sockaddr *dest, socklen_t dlen) {
    if (up_bytes_sent + (int)len > UP_BUDGET) return -1;
    ssize_t r = sendto(fd, buf, len, 0, dest, dlen);
    if (r > 0) up_bytes_sent += (int)r;
    return (int)r;
}

/* Build and send a DATA packet */
static void send_data(int fd, uint32_t seq, const uint8_t *payload,
                      const struct sockaddr *relay, socklen_t rlen) {
    uint8_t pkt[1 + 4 + PAYLOAD_LEN];
    pkt[0] = PKT_DATA;
    uint32_t net_seq = htonl(seq);
    memcpy(pkt + 1, &net_seq, 4);
    memcpy(pkt + 5, payload, PAYLOAD_LEN);
    budget_send(fd, pkt, sizeof(pkt), relay, rlen);
}

/* Build and send a FEC parity packet */
static void send_fec(int fd, uint32_t group_start, const uint8_t *parity,
                     uint8_t group_size,
                     const struct sockaddr *relay, socklen_t rlen) {
    uint8_t pkt[1 + 4 + PAYLOAD_LEN + 1];
    pkt[0] = PKT_FEC;
    uint32_t net_gs = htonl(group_start);
    memcpy(pkt + 1, &net_gs, 4);
    memcpy(pkt + 5, parity, PAYLOAD_LEN);
    pkt[5 + PAYLOAD_LEN] = group_size;
    budget_send(fd, pkt, sizeof(pkt), relay, rlen);
}

int main(void) {
    /* Parse env vars */
    const char *dur_s = getenv("DURATION_S");
    double duration = dur_s ? atof(dur_s) : 30.0;
    int n_frames = (int)(duration * 1000) / 20;
    UP_BUDGET = (int)(1.8 * n_frames * PAYLOAD_LEN);

    /* Socket: receive from harness source (port 47010) */
    int src_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in src_addr = {0};
    src_addr.sin_family = AF_INET;
    src_addr.sin_port = htons(47010);
    src_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(src_fd, (struct sockaddr *)&src_addr, sizeof(src_addr)) < 0) {
        perror("bind 47010");
        return 1;
    }
    fcntl(src_fd, F_SETFL, O_NONBLOCK);

    /* Socket: receive NACKs from receiver via relay (port 47004) */
    int fb_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in fb_addr = {0};
    fb_addr.sin_family = AF_INET;
    fb_addr.sin_port = htons(47004);
    fb_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(fb_fd, (struct sockaddr *)&fb_addr, sizeof(fb_addr)) < 0) {
        perror("bind 47004");
        return 1;
    }
    fcntl(fb_fd, F_SETFL, O_NONBLOCK);

    /* Socket: send to relay uplink (port 47001) */
    int out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in relay = {0};
    relay.sin_family = AF_INET;
    relay.sin_port = htons(47001);
    relay.sin_addr.s_addr = inet_addr("127.0.0.1");

    memset(fec_parity, 0, PAYLOAD_LEN);
    memset(retx_buf, 0, sizeof(retx_buf));

    unsigned char buf[2048];
    int max_fd = (src_fd > fb_fd) ? src_fd : fb_fd;

    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(src_fd, &rfds);
        FD_SET(fb_fd, &rfds);

        struct timeval tv = {0, 50000}; /* 50ms timeout */
        int ready = select(max_fd + 1, &rfds, NULL, NULL, &tv);
        if (ready <= 0) continue;

        /* Handle harness source frame */
        if (FD_ISSET(src_fd, &rfds)) {
            for (;;) {
                ssize_t n = recvfrom(src_fd, buf, sizeof(buf), 0, NULL, NULL);
                if (n < 0) break;
                if (n == 4 + PAYLOAD_LEN) {
                    /* Parse harness frame: 4B BE seq + 160B payload */
                    uint32_t seq;
                    memcpy(&seq, buf, 4);
                    seq = ntohl(seq);
                    const uint8_t *payload = buf + 4;

                    /* Store in retransmit buffer */
                    int slot_idx = seq % RETX_BUF_SIZE;
                    retx_buf[slot_idx].seq_tag = seq;
                    memcpy(retx_buf[slot_idx].payload, payload, PAYLOAD_LEN);
                    retx_buf[slot_idx].retransmit_count = 0;
                    retx_buf[slot_idx].valid = 1;

                    /* Send DATA packet */
                    send_data(out_fd, seq, payload,
                              (struct sockaddr *)&relay, sizeof(relay));

                    /* FEC accumulation */
                    if (fec_count == 0) {
                        fec_group_start = seq;
                        memset(fec_parity, 0, PAYLOAD_LEN);
                    }
                    for (int j = 0; j < PAYLOAD_LEN; j++) {
                        fec_parity[j] ^= payload[j];
                    }
                    fec_count++;

                    if (fec_count == FEC_N) {
                        /* Group complete, send parity */
                        send_fec(out_fd, fec_group_start, fec_parity, FEC_N,
                                 (struct sockaddr *)&relay, sizeof(relay));
                        fec_count = 0;
                    }

                    /* Partial final group */
                    if ((int)seq == n_frames - 1 && fec_count > 0 && fec_count < FEC_N) {
                        send_fec(out_fd, fec_group_start, fec_parity,
                                 (uint8_t)fec_count,
                                 (struct sockaddr *)&relay, sizeof(relay));
                        fec_count = 0;
                    }
                }
            }
        }

        /* Handle NACK from receiver */
        if (FD_ISSET(fb_fd, &rfds)) {
            for (;;) {
                ssize_t n = recvfrom(fb_fd, buf, sizeof(buf), 0, NULL, NULL);
                if (n < 0) break;
                if (n == 5 && buf[0] == PKT_NACK) {
                    uint32_t nack_seq;
                    memcpy(&nack_seq, buf + 1, 4);
                    nack_seq = ntohl(nack_seq);

                    int slot_idx = nack_seq % RETX_BUF_SIZE;
                    struct retx_slot *slot = &retx_buf[slot_idx];
                    if (slot->valid && slot->seq_tag == nack_seq &&
                        slot->retransmit_count < MAX_RETX) {
                        send_data(out_fd, nack_seq, slot->payload,
                                  (struct sockaddr *)&relay, sizeof(relay));
                        slot->retransmit_count++;
                    }
                }
            }
        }
    }
    return 0;
}
