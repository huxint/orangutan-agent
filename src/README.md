# Source

`src/main.cpp` is the executable argument boundary. Each `src/oran-<lib>` owns one
library with public headers under `include/oran/<lib>`. Private implementation
headers stay under that library. [Architecture](../docs/ARCHITECTURE.md) owns
responsibilities and dependency direction.
