# MM HTTP Bridge

Expose a local-only HTTP server that returns game state and accepts message requests.

## Endpoints

- `GET /v1/health`
- `GET /v1/state`
- `POST /v1/message`

All requests must include `X-Api-Key` matching the mod config.

## Example

```
curl -H "X-Api-Key: changeme" http://127.0.0.1:6464/v1/health
curl -H "X-Api-Key: changeme" http://127.0.0.1:6464/v1/state
curl -H "X-Api-Key: changeme" -d '{"text":"Hello from HTTP"}' http://127.0.0.1:6464/v1/message
```

## Config

- `server_enabled`: Off/On
- `bind_address`: default `127.0.0.1`
- `port`: default `6464`
- `api_key`: default `changeme`
- `snapshot_rate`: frames between state updates
- `max_message_length`: max chars for `/v1/message`
