# HIL fixture contract

## 1. Identity

- Device name: `UniversalBLE-HIL`
- Manufacturer ID bytes: `57 7e`
- Manufacturer payload: contract revision `02 00`
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

The first byte is the opcode and the remaining bytes are its payload.
All 16-bit integers are little-endian. The legacy BLE transport writes those
bytes directly to the control characteristic.

For UART, open the DK's virtual COM port at 115200 baud with DTR asserted.
Send one LF-terminated line per request: eight hexadecimal request-ID digits,
a space, and the command bytes encoded as hex. For example, `00000001 01`
requests a fixture reset (not a reboot). Wait for the matching response
`HIL <id> <status>` before sending another request. Status zero means the
fixture accepted the command; a negative status means the fixture rejected
it. Acceptance does not prove a scheduled action fired.
Unrelated output is Zephyr logging. A UART-only opcode `ff` with no payload
returns `HIL <id> READY 2 <BLE address>`.
UART-only opcode `fe` with no payload returns `HIL <id> TELEMETRY <hex>`,
where the hex encodes the same 36 bytes as the BLE telemetry characteristic.
It can be read with no BLE connection.

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
| `0c`   | Schedule auxiliary service | Enabled byte and delay, `uint16`        | Change the auxiliary service after the requested delay |
| `0d`   | Arm CCC delay       | Delay in milliseconds, `uint16`              | Delay the next notification CCC callback             |
| `0e` | Arm descriptor read fault | Same five-byte fault payload | Alter the next descriptor read |
| `0f` | Arm descriptor write fault | Same five-byte fault payload | Alter the next descriptor write |
| `10` | Indicate on subscribe | None | Send `IND-CCC-ENABLED` inside the next indication CCC-enable callback |
| `11` | Indication scenario | Action byte: 1 disconnect, 2 reboot, 3 competing send | Queue `IND-FIRST`, then act before this RX callback can process its confirmation |
| `12` | Arm CCC fault | Action (1 disconnect, 2 reboot), trigger delay `uint16`, callback delay `uint16` | One-shot fault starts inside the next notification CCC callback, not during setup |
| `13` | Set long value | Length `uint16`, 0..512 | Fill the read value with byte `index & 0xff` without sending a large control write |
| `14` | Notification pressure | None | Attempt 128 notification sends with completion callbacks in one system-workqueue invocation |
| `15` | Arm next connection fault | Delay `uint16` | One-shot plan survives host disconnect and starts its disconnect timer inside the next connection callback |
| `16` | Arm service-declaration fault | Five-byte fault payload | Alter the next read of the HIL primary-service declaration, including uncached service discovery |

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

`Schedule auxiliary service` lets a Service Changed indication overlap an
active read, write, or subscription operation. `Arm CCC delay` is one-shot and
delays the next notification enable or disable callback.

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
- Armed CCC delay: cleared

CCC flags continue to reflect the active connection because reset does not unsubscribe the client.

## 5. Revision 2 telemetry and long values

The read-only telemetry characteristic is `7e57000f-7e57-4e57-8e57-7e5700000001`.
It returns 36 bytes, all integers little-endian:

| Offset | Type | Meaning |
| ------ | ---- | ------- |
| 0 | uint16 | Indication parameters still owned by Zephyr (0 or 1) |
| 2 | uint16 | Successfully confirmed indications |
| 4 | uint16 | Competing sends rejected while parameters were busy |
| 6 | uint16 | Indication scenario triggered while an indication was pending |
| 8 | uint16 | CCC fault callbacks triggered |
| 10 | uint32 | Boot count retained in no-init RAM across software reboot |
| 14 | uint16 | Oversized writes rejected by the fixture |
| 16 | uint16 | Pressure notifications accepted |
| 18 | uint16 | Pressure sends rejected with `-ENOMEM` |
| 20 | uint16 | Pressure notification completion callbacks |
| 22 | uint16 | Other pressure-send errors (test expects zero) |
| 24 | uint16 | Next-connection fault callbacks triggered |
| 26 | uint16 | Service-declaration fault callbacks triggered |
| 28 | uint16 | Physical disconnect callbacks since reset |
| 30 | uint16 | Disconnect timers armed since reset |
| 32 | uint16 | Disconnect timer attempts on the retained original connection |
| 34 | uint16 | Disconnect timers cancelled before execution |

Disconnect timers retain the connection that was active when armed. Closing
that link cancels its pending timer; work already executing can only target its
retained original link. It cannot disconnect a replacement connection. Reset
cancels pending timers and clears all three timer counters. Rescheduling a timer
replaces the previous plan; the counter accounting used by CONN-007 assumes one
armed plan per reset.

Reset clears the event counters and fault plans, but never clears an indication
pending flag while Zephyr still owns its parameters. Only the indication destroy
callback releases that ownership. The boot count is diagnostic, not persistent
storage: it is not guaranteed to survive power loss or reflashing.

Read values now accept up to 512 bytes. The write-with-response value and its
mirror have a deliberately smaller 384-byte limit, allowing a valid 385-byte
ATT write to demonstrate peripheral rejection without exceeding the protocol's
512-byte limit. The telemetry counter proves that rejection reached the fixture.
The write-with-response
characteristic validates Prepare Writes without modifying the mirror; data is
stored only when Zephyr executes the prepared write. Notification bursts remain
limited to 244 bytes. Oversized writes return an ATT length error.

Timing sweeps use 300 ms blocked callbacks and disconnect offsets of 0, 25, 100,
and 350 ms. The timer is started by the target callback, so BLE setup latency
does not shift the intended fault point. The 350 ms case is after callback return;
host completion can still lose the race with transport delivery of the response.

## 6. Explicit unattended limitations

This profile does not implement SMP, encrypted attributes, persistent bonds or
passkey control. Windows pairing consent is not automated. SMP cases are skipped
with a reason, not counted as implemented tests.

Windows MTU queries read local metadata, without a server callback to interrupt.
The public host API cannot retain raw Prepare Writes to exhaust a server queue.
Successful bursts alone do not prove buffer exhaustion. The pressure command
uses `bt_gatt_notify_cb` from the system workqueue, where resource allocation is
nonblocking and completion callbacks cannot run until the batch returns. The
test requires observed `-ENOMEM`, accepted sends, completion of every accepted
send, exact received payloads, and subsequent successful notifications and GATT.

The next-connection fault is armed over BLE before host disconnect. Discovery
faults use a custom primary-service declaration read callback that delegates to
Zephyr's normal service reader after applying the one-shot plan. Windows tests
exercise uncached discovery inside `connect`; public `discoverServices` can
return cached metadata without reading the declaration over the radio.
The connection/discovery sweep waits for fixture advertising before its next
connect attempt. Windows can retry a connection before Dart emits a connected
event; the test therefore checks that the fixture saw the fault and at least
two physical disconnects (the deliberate host disconnect plus the fault), then
checks a usable connection. A Dart `false` event is not required for a failed
attempt that was never exposed as connected.
