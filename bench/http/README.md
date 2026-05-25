# `bench-http`

HTTP benchmarks pin the body-client boundary cost before provider/bootstrap
wire it into real adapter construction.

| Scenario | What it compares |
| --- | --- |
| [`scenarios/client.cpp`](scenarios/client.cpp) | `http.validate_body_request` measures local request validation, while `http.construct_client` is the construction baseline for the pimpl/curl-global boundary. |
