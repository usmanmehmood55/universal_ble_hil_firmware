# HIL fixture contract

## 1. Identity

- Device name: `UniversalBLE-HIL`
- Manufacturer ID bytes: `57 7e`
- Manufacturer payload: contract revision `01 00`
- Service UUID: `7e570001-7e57-4e57-8e57-7e5700000001`

All characteristic UUIDs share that base and change the first field:

| Suffix | Purpose                           | Properties             |
| ------ | --------------------------------- | ---------------------- |
| `0002` | Control                           | Write                  |
| `0003` | State                             | Read                   |
| `0004` | Deterministic value               | Read                   |
| `0005` | Write with response               | Write                  |
| `0006` | Write without response            | Write without response |
| `0007` | Last write-with-response value    | Read                   |
| `0008` | Last write-without-response value | Read                   |
| `0009` | Test notifications                | Notify                 |
| `000a` | Test indications                  | Indicate               |
| `000b` | Multi-property value              | Read, write, notify    |

For example, the notification UUID is
`7e570009-7e57-4e57-8e57-7e5700000001`.

## 2. Control commands

Commands are binary writes. The first byte is the opcode and the remaining
bytes are its payload. All 16-bit integers are little-endian.

| Opcode | Name                | Payload                                      | Effect                                             |
| ------ | ------------------- | -------------------------------------------- | -------------------------------------------------- |
| `01`   | Reset               | None                                         | Restore values and counters; cancel scheduled work |
| `02`   | Set read value      | Arbitrary bytes                              | Replace the deterministic read value               |
| `03`   | Notify              | Arbitrary bytes                              | Emit one notification with exactly this payload    |
| `04`   | Indicate            | Arbitrary bytes                              | Emit one indication with exactly this payload      |
| `05`   | Disconnect          | Delay in milliseconds, `uint16`              | Peripheral-initiated disconnect after the delay    |
| `06`   | Notification burst  | Count, size, interval; three `uint16` values | Emit sequenced notifications                       |
| `07`   | Arm read fault      | ATT error, delay, disconnect delay           | Alter the next deterministic-value read            |
| `08`   | Arm write fault     | ATT error, delay, disconnect delay           | Alter the next write-with-response                 |
| `09`   | Notification script | Size, interval, count, sequence numbers      | Emit caller-selected application sequence numbers  |
| `0a`   | Notify on subscribe | None                                         | Emit `CCC-ENABLED` from the next notification CCC enable callback |
| `0b`   | Set auxiliary service | One byte: `00` disabled or `01` enabled    | Dynamically remove or add the auxiliary GATT service |

The fault-command payload is five bytes: one ATT error byte, a `uint16`
callback delay in milliseconds, and a `uint16` peripheral-disconnect delay. An
ATT error of zero lets the operation succeed. A disconnect delay of `ffff`
disables disconnection. Each fault is consumed by exactly one matching
operation, and reset disarms both faults.

Burst notifications contain a little-endian sequence number in bytes 0 and 1.
Remaining bytes equal their zero-based byte offsets. Sizes range from 2 through
244 bytes. The firmware retries the current sequence number when Zephyr
reports transient notification-buffer backpressure, so an accepted burst
command produces exactly the requested number of frames.

The notification-script payload starts with a `uint16` payload size and a
`uint16` interval in milliseconds, followed by a one-byte count and exactly
that many little-endian `uint16` sequence numbers. The count ranges from 1
through 64. Scripted notifications use the same payload format and retry
behavior as an ordinary burst, but emit the supplied sequence numbers exactly,
including gaps, duplicates, and non-monotonic order.

Commands which need a CCC subscription fail with an ATT error when that
subscription is disabled.

`Notify on subscribe` is one-shot. It sends from the firmware's CCC callback,
before the CCC write has completed on the client. This exercises clients which
register their notification handler too late in the subscription sequence.

`Set auxiliary service` is acknowledged before delayed work changes the GATT
database. Zephyr emits the standard Service Changed indication. The auxiliary
service UUID ends in `000c`; its readable characteristic UUID ends in `000d`
and returns `AUXILIARY-V1`.

## 3. State value

The state characteristic returns 16 bytes:

| Offset | Size | Meaning                            |
| ------ | ---- | ---------------------------------- |
| 0      | 1    | Contract revision                  |
| 1      | 1    | Notification CCC enabled           |
| 2      | 1    | Indication CCC enabled             |
| 3      | 1    | Multi-property CCC enabled         |
| 4      | 4    | Successful writes with response    |
| 8      | 4    | Successful writes without response |
| 12     | 2    | Current ATT MTU                    |
| 14     | 2    | Reserved, zero                     |

## 4. Reset state

- Read value: UTF-8 `HIL-READ-V1`
- Multi-property value: UTF-8 `MULTI-V1`
- Write mirrors: empty
- Write counters: zero
- Scheduled disconnect and burst work: cancelled
- Armed read and write faults: cleared
- Notify-on-subscribe plan: cleared
- Auxiliary service: removed through delayed work

CCC flags continue to reflect the active connection because reset does not unsubscribe the client.
