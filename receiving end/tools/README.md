# WS63 temporary HTTP test server

`ws63_range_server.py` is a dependency-free static file server for testing
WS63 downloads. Unlike `python3 -m http.server`, it answers a single
`Range: bytes=N-` request with `206 Partial Content` and `Content-Range`.

On the Ubuntu test server:

```bash
mkdir -p "$HOME/ws63-web"
cp ws63-test.mp3 "$HOME/ws63-web/"
nohup python3 ws63_range_server.py \
  --bind 0.0.0.0 \
  --port 18080 \
  --directory "$HOME/ws63-web" \
  > "$HOME/ws63-http.log" 2>&1 &
echo $! > "$HOME/ws63-http.pid"
```

Verify the range response from a client that can reach the VM:

```bash
curl -r 4194304- -o /dev/null \
  -w 'http=%{http_code} bytes=%{size_download} content_range=%header{content-range}\n' \
  http://124.222.12.152:18080/ws63-test.mp3
```

For older curl versions, use `-D -` instead of `%header{content-range}` to
inspect the response headers. The expected result for the current file is
HTTP `206` and `4901792` bytes.
