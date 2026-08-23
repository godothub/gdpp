# Godot GDScript AOT & Extension

**GDPP** compiles GDScript code into native binaries:

- High performance: as fast as C/C++
- Cross-platform: supports all major platforms
- Easy to use: one-click export after enabling the plugin
- Auto encryption: protects your assets as much as possible
- Syntax extensions: a more powerful GDScript language

## Performance Results

| Workload | GDScript | GDPP AOT | Relative performance (GDScript = 100%) |
| --- | ---: | ---: | ---: |
| 2D workload | 18.350 μs/frame | 0.150 μs/frame | 12,233.33% |
| 500×500 matrix multiplication with `Array[int]` | 3.207 s | 29.644 ms | 10,819.09% |
| 500×500 matrix multiplication with `PackedInt64Array` | 2.654 s | 25.631 ms | 10,356.40% |

> Note: These results were measured on a Mac mini with a 10-core M4 and 16 GB of memory, running macOS 26.6.2 with the official Godot 4.7.1 editor and export templates. The benchmarks are provided for reference and do not represent every real-world project.
