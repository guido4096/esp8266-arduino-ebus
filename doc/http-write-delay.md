# Optional HTTP write delay

Clients that generate several successive changes to the same setting can
combine them into one bus write. For example, a slider or a script can submit
intermediate values while the firmware waits for the requests to stop before
dispatching the last value.

`POST /api/v1/app/values/write` accepts an optional `delay_ms` integer,
from 0 to 60000 milliseconds. Omitting it or passing zero retains immediate
queuing and the existing HTTP 200 response.

```json
{"key":"room_setpoint","value":21.5,"delay_ms":2000}
```

This works with every supported write type. The option belongs to the HTTP
request, not to the saved command definition. No command JSON upload is needed.
Replace `room_setpoint` with the key of a configured writable command.

Each valid delayed request replaces the waiting value for the same key and
restarts its delay, including requests that repeat the same value. Different
keys have independent deadlines. Processing runs every 100 ms, so dispatch
can occur slightly after the deadline. Bus arbitration can add further delay.

Writes use the latest accepted value for each command key. Accepting a newer
request cancels an older queued write or retry for that key, even while the
new value is waiting for its delay to expire. An active attempt may finish,
but will not retry after being superseded. It cannot execute after the newer
telegram because the bus handles one active transaction at a time.

An accepted delayed request returns HTTP 202:

```json
{"id":"write","status":"scheduled"}
```

This confirms acceptance into the firmware's pending-write queue, not delivery
to the bus or confirmation by the target device. The queue holds at most
`WRITE_CMD_CAPACITY` distinct keys (16 by default). Replacing an existing key
does not consume another slot. A full pending queue returns HTTP 503.
Invalid values or delays are rejected without changing a waiting request.

After the delay, the latest telegram is queued once. If the eBUS scheduler
rejects it, the firmware logs the failure and discards that pending request.
There are no additional application retries; normal library retries still
apply to accepted telegrams. Readback is requested only after the library
reports successful completion of the current write, not when it is queued or
when HTTP 202 is returned. Errors and late completions of superseded writes
do not trigger readback. Commands without an active read definition cannot
request a readback. Check the actual value through normal polling.

An accepted immediate write for the same key cancels a waiting delayed write.
This also applies to immediate MQTT and cron writes. A rejected immediate
write leaves the pending request intact. Replacement in the library queue is
atomic: rejection of a new immediate request leaves the previous bus session
intact too. Removing or replacing a command, clearing the command set, or
stopping the application for an upgrade cancels waiting writes and prevents
retries of active writes. Pending writes are not persisted across restarts.

The delay reduces bus writes, not HTTP requests. It does not distinguish user
actions from feedback: clients that submit requests in response to device
updates can still cause redundant same-value writes. Confirmation tracking
and feedback suppression are separate concerns.

## Example client: Loxone

For a Loxone analogue HTTP output, include the delay in its POST body:

```text
{"key":"room_setpoint","value":<v.1>,"delay_ms":2000}
```

Loxone replaces `<v.1>` with the output value before sending the JSON request.
Other HTTP clients use the same API with their own value substitution.
