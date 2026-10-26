# Write diagnostics

Open **Logs**, leave **INFO** enabled and select **Writes only** to follow
write requests. The filter applies to live updates and downloaded logs.
All diagnostic messages start with `[WRITE]`.

For example:

```text
[WRITE] req=10 key=room_setpoint event=received value=21.5 delay_ms=2000
[WRITE] req=10 key=room_setpoint event=waiting
[WRITE] req=11 key=room_setpoint event=received value=22 delay_ms=2000
[WRITE] req=11 key=room_setpoint event=replaced previous_req=10 delay_ms=2000
[WRITE] req=11 key=room_setpoint event=bus_queued telegram=...
```

`req` identifies a write request within the current boot. A `replaced` line
identifies the waiting request that was superseded. The SID column links
`bus_queued` to subsequent `bus_error` and `bus_success` messages. The queued
telegram is logged in hex so its actual bytes can be inspected.

- `received`: a valid request reached the write scheduler, including its value
  and requested delay. HTTP, MQTT and cron writes use the same diagnostics.
- `waiting` / `replaced`: the request is waiting for its delay to expire.
- `bus_queued`: the eBUS library accepted the telegram. It has not necessarily
  been transmitted yet.
- `rejected`: either the delayed-write queue or the eBUS queue rejected it.
- `cancelled`: a waiting request was cancelled, for example by an immediate
  write, command replacement/removal, or application shutdown.
- `bus_error`: an attempt produced an error. Retries remain library-managed;
  this line alone does not establish terminal failure.
- `bus_success`: the library reported a completed telegram. This does not
  establish that the target device stored the requested setting.
- `read_requested` / `read_queue_rejected`: a readback was requested or its
  enqueue failed. These messages carry the read session ID in the SID column.
- `readback`: a configured writable command received new data. Its decoded
  value is logged with the read session ID when available. `req=0` means this
  observation is not assigned to a particular write request; periodic reads
  and externally generated updates can appear here too.

To investigate a missing final value, compare the last `received` value with
the next `bus_queued` telegram and subsequent readbacks for that key. A later
`received` line containing an older value can expose a client feedback loop.

Result correlation uses bounded diagnostic history (twice
`WRITE_CMD_CAPACITY`, 32 sessions by default). Eviction affects logging only.
The INTERNAL build retains 32 log entries, shared with other diagnostics.
Keep the Logs page open during the
test and download the capture promptly. Missing result lines alone are not
proof of a failed write, because diagnostic events can be dropped or evicted.

Formatting runs on application and monitor tasks, not in bus interrupts or
the protocol callback. Logging does not add retries or confirmation tracking.
