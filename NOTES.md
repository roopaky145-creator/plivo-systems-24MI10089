# Architecture and Design Choices

## Overview
This project implements a robust real-time communication sender and receiver pair that handles jitter, packet loss, and latency over a hostile UDP relay. The goal is to maximize the number of packets arriving at the player before their strict deadlines, while keeping bandwidth overhead under 2.0x.

## 1. Hybrid FEC and NACK Strategy
To handle packet loss, the system uses a hybrid approach of Forward Error Correction (FEC) and NACK-based retransmissions.

### Forward Error Correction (FEC)
- We use an XOR-based parity scheme with `FEC_N = 2`. 
- For every 2 DATA packets sent, the sender automatically generates and sends 1 FEC parity packet. 
- This immediately recovers isolated packet drops without requiring a round-trip delay, which is critical for meeting tight deadlines (e.g., Profile A's 80ms delay).
- The `FEC_N=2` parameter was chosen because `FEC_N=3` or higher requires waiting too long for the entire group to accumulate, which pushes the parity packet past the deadline. `FEC_N=2` keeps recovery fast while maintaining an acceptable baseline overhead (1.5x).

### NACK-based Retransmission
- For burst losses that FEC cannot recover, the receiver falls back to sending NACK (Negative Acknowledgment) requests back to the sender.
- **Timing Guards:** NACKs are carefully gated by two timing checks:
  1. **Grace Period:** The receiver waits for a fraction of the delay (`delay_ms * 0.5`, capped at 50ms) before requesting a NACK. This gives the original packet and its FEC parity time to arrive, preventing duplicate transmissions.
  2. **Guard Period:** The receiver checks if there is enough time remaining before the deadline to complete a full round-trip. We assume a minimum RTT of 20ms. If the deadline is closer than 20ms, the NACK is suppressed to save bandwidth.

## 2. Receiver Jitter Buffer & Playout
The receiver implements a single-threaded non-blocking event loop rather than a multi-threaded approach. This avoids OS context-switching stalls (especially on WSL) which can lead to blocks of late playouts.

- **Jitter Buffer:** A circular array of 2048 slots tracks received payloads and sequence numbers.
- **Event Loop:** The receiver uses `select()` with a dynamically calculated timeout to wake up precisely before the next scheduled playout deadline.
- **Non-blocking Sockets:** All sockets are set to non-blocking. Before advancing the playout clock, the receiver completely drains the kernel's UDP incoming buffer. This ensures that any packet that has physically arrived is processed and credited *before* its deadline expires.

## 3. Sender Budgeting and Flow Control
The sender maintains a 2048-slot circular buffer of previously transmitted packets.
- When a NACK is received, it verifies that the packet is still valid in the history buffer and that it hasn't exceeded a maximum retransmission count (3 times).
- The receiver also tracks the `DOWN_BUDGET` (20% of total payload bytes) and stops sending NACKs if the bandwidth limit is near, ensuring we never cross the 2.0x total overhead cap.

## 4. Custom Wire Protocol
To keep bandwidth low, we use a custom packed binary protocol:
- **DATA Packet (165 bytes):** `[Type: 1B = 0x01][SeqNo: 4B BE][Payload: 160B]`
- **FEC Packet (166 bytes):** `[Type: 1B = 0x02][GroupStartSeq: 4B BE][Parity: 160B][GroupSize: 1B]`
- **NACK Packet (5 bytes):** `[Type: 1B = 0x03][MissedSeq: 4B BE]`
