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

## Plugin Support

Use the **GDPP Plugins** panel or a YAML configuration to export a GDScript plugin as a compiled GDPP plugin for use in other Godot projects. Using the exported plugin requires neither GDPP nor a C++ compiler. The package retains `.gd` interface files for resource references and public APIs; plugin logic runs in the native library.

When exporting a whole project, GDPP compiles project scripts outside `addons/`. To compile a plugin, export it separately.

### Export in the editor

Enable GDPP and open the **GDPP Plugins** bottom panel. Select an installed plugin or enter an external source directory. Edit the generated YAML to choose targets and compilers, import or save a configuration, then click **Inspect** to view its configuration and script inventory or **Export** to compile and package it. The target field overrides `build.targets`; separate names with commas.

The panel works in macOS, Windows, and Linux editors without a separate `gdpp` command-line installation. Install the compiler or toolchain required by your target platform before exporting.

### Export from the command line

Download `gdpp-compiler.zip` from Release. Keep its `sdk/`, `tools/`, and license directories alongside the platform directories. Run the program for your system, or add its directory to `PATH`.

```text
linux/x86_64/gdpp          # glibc 2.35 or later
windows/x86_64/gdpp.exe    # Windows 10 or later
```

Save the example below as `my_plugin.yaml`, set your plugin directory, target platforms, and compilers, then run:

```sh
gdpp inspect my_plugin.yaml
gdpp build my_plugin.yaml --targets windows-msvc
```

```yaml
schema_version: 1
plugin:
  source: ./addons/my_plugin
godot:
  executable: godot
  # path: /opt/godot
  target_version: "4.7"
build:
  targets: [windows-msvc]
output:
  directory: ./dist
targets:
  windows-msvc:
    platform: windows
    architecture: x86_64
    compiler: cl.exe
  macos:
    platform: macos
    architecture: universal
    compiler: clang++
  android:
    platform: android
    architecture: arm64
    ndk: /opt/android-ndk
```

- Set `plugin.source` to your source plugin directory and `output.directory` to the export directory. Relative paths resolve from the YAML directory.
- Set `godot.executable` to the Godot program name. Omit `path` when it is in `PATH`; otherwise, set it to the executable's directory. Set `target_version` to the target Godot version, such as `"4.7"`.
- Configure your target platforms under `targets`. Set `compiler` to a full path or a program name in `PATH`; on Windows, use `cl.exe` or MinGW64's `g++.exe`. For Android, set the `ndk` directory.
- Select target names with `build.targets` or the command's `--targets` option. Separate multiple names with commas, for example `--targets macos,android`.

Each export target needs its toolchain: MSVC uses Windows, macOS/iOS use macOS and Xcode, Android uses the NDK, and Web uses Emscripten. Exporting preserves your original plugin.

## Crossing Boundaries

GDScript code that makes frequent engine calls may not see a significant performance improvement. We are also optimizing GDPP's compilation process to reduce boundary crossings as one way to improve performance.

## License

1. The code in this repository is GDPP Community Edition. It is fully open source and free under the [MIT License](https://github.com/godothub/gdpp/blob/main/LICENSE).
2. GDPP Community Edition uses the `1.x` version series, while Professional Edition uses `2.x`. Professional Edition is free for individual users, including small indie game teams that are not organized as companies. Companies, organizations, and institutions should [contact us](mailto:contact@godothub.com).
