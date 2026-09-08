# Godot GDScript AOT & Extension

**GDPP** compiles GDScript into binaries:

- High performance: compiles to native code and binaries
- Cross-platform: supports desktop, mobile, and Web platforms
- Easy to use: one-click export after enabling the plugin
- Syntax extensions: a more powerful GDScript language

## Performance Results

| Workload | GDScript | GDPP AOT | Relative performance |
| --- | ---: | ---: | ---: |
| Matrix multiplication | 3.064 s | 30.397 ms | 10,080.53% |
| 2D test case | 17.776 μs/frame | 0.140 μs/frame | 12,697.14% |

> Note: These results were measured on a Mac mini with a 10-core M4 and 16 GB of memory, running macOS 26.6.2 with the official Godot 4.7.2 editor and export templates.

## Quick Start

1. Extract the plugin to `addons/gdpp`, then enable it.
2. Make sure a C++ compiler is installed locally, such as MSVC on Windows.
3. Export the project and wait for the compilation progress bar to complete.

## Syntax Extensions

GDPP targets compatibility with official GDScript and plans to support additional syntax. If you encounter compatibility issues or have suggestions for future GDScript syntax enhancements, please share them through an [issue](https://github.com/godothub/gdpp/issues).

## Crossing Boundaries

GDScript code that makes frequent engine calls may not see a significant performance improvement. We are also optimizing GDPP's compilation process to reduce boundary crossings as one way to improve performance.

## License

1. The code in this repository is GDPP Community Edition. It is fully open source and free under the [MIT License](https://github.com/godothub/gdpp/blob/main/LICENSE).
2. GDPP Community Edition uses the `1.x` version series, while Professional Edition uses `2.x`. Professional Edition is free for individual users, including small indie game teams that are not organized as companies. Companies, organizations, and institutions should [contact us](mailto:contact@godothub.com).
