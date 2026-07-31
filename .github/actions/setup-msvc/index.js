"use strict";

const childProcess = require("node:child_process");
const fs = require("node:fs");
const path = require("node:path");

const PATH_VARIABLES = new Set(["PATH", "INCLUDE", "LIB", "LIBPATH"]);
const ARCHITECTURE_ALIASES = new Map([
    ["win64", "x64"],
    ["x86-64", "x64"],
    ["x86_64", "x64"],
]);

function workflowEscape(value) {
    return String(value).replaceAll("%", "%25").replaceAll("\r", "%0D").replaceAll("\n", "%0A");
}

function fail(message) {
    process.stderr.write(`::error::${workflowEscape(message)}\n`);
    process.exitCode = 1;
}

function resolveArchitecture(value) {
    const requested = String(value || "x64").trim().toLowerCase();
    const architecture = ARCHITECTURE_ALIASES.get(requested) || requested;
    if (architecture !== "x64") {
        throw new Error(
            `unsupported MSVC architecture '${value}'; GDPP currently ships Windows x64 only`,
        );
    }
    return architecture;
}

function parseEnvironment(output) {
    const environment = new Map();
    for (const rawLine of String(output).split(/\r?\n/u)) {
        const separator = rawLine.indexOf("=");
        if (separator <= 0) {
            continue;
        }
        const name = rawLine.slice(0, separator);
        const value = rawLine.slice(separator + 1);
        if (/[\r\n=]/u.test(name)) {
            throw new Error(`MSVC emitted an invalid environment variable name: ${name}`);
        }
        environment.set(name.toUpperCase(), { name, value });
    }
    return environment;
}

function deduplicatePath(value) {
    const seen = new Set();
    return String(value)
        .split(";")
        .filter((entry) => {
            const identity = entry.toLowerCase();
            if (seen.has(identity)) {
                return false;
            }
            seen.add(identity);
            return true;
        })
        .join(";");
}

function changedEnvironment(before, after) {
    const previous = new Map(
        Object.entries(before).map(([name, value]) => [
            name.toUpperCase(),
            value === undefined ? "" : String(value),
        ]),
    );
    const changes = [];
    for (const [identity, entry] of after) {
        if (previous.get(identity) === entry.value) {
            continue;
        }
        const value = PATH_VARIABLES.has(identity) ? deduplicatePath(entry.value) : entry.value;
        if (/[\r\n]/u.test(value)) {
            throw new Error(`MSVC emitted a multiline value for ${entry.name}`);
        }
        changes.push({ name: entry.name, value });
    }
    return changes.sort((left, right) => left.name.localeCompare(right.name));
}

function findVisualStudio() {
    const programFilesX86 = process.env["ProgramFiles(x86)"];
    if (!programFilesX86) {
        throw new Error("ProgramFiles(x86) is unavailable on the Windows runner");
    }
    const vswhere = path.join(
        programFilesX86,
        "Microsoft Visual Studio",
        "Installer",
        "vswhere.exe",
    );
    if (!fs.existsSync(vswhere)) {
        throw new Error(`Visual Studio locator is unavailable: ${vswhere}`);
    }
    const result = childProcess.spawnSync(
        vswhere,
        [
            "-latest",
            "-products",
            "*",
            "-requires",
            "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
            "-property",
            "installationPath",
        ],
        {
            encoding: "utf8",
            windowsHide: true,
        },
    );
    if (result.error) {
        throw new Error(`failed to execute vswhere: ${result.error.message}`);
    }
    if (result.status !== 0) {
        throw new Error(`vswhere exited with status ${result.status}: ${result.stderr.trim()}`);
    }
    const installation = result.stdout
        .split(/\r?\n/u)
        .map((line) => line.trim())
        .find(Boolean);
    if (!installation) {
        throw new Error("Visual Studio with the x64 C++ toolchain is unavailable");
    }
    const vcvars = path.join(installation, "VC", "Auxiliary", "Build", "vcvarsall.bat");
    if (!fs.existsSync(vcvars)) {
        throw new Error(`MSVC environment bootstrap is unavailable: ${vcvars}`);
    }
    return { installation, vcvars };
}

function captureEnvironment(
    vcvars,
    architecture,
    spawn = childProcess.spawnSync,
    commandProcessor = process.env.ComSpec || "C:\\Windows\\System32\\cmd.exe",
) {
    const command = `call "${vcvars}" ${architecture} >nul && set`;
    const result = spawn(commandProcessor, ["/d", "/u", "/s", "/c", command], {
        encoding: "utf16le",
        maxBuffer: 16 * 1024 * 1024,
        windowsHide: true,
        // cmd.exe owns the payload grammar after /c. Node's generic Windows argument quoting
        // escapes the quotes around Program Files as \", which cmd treats literally.
        windowsVerbatimArguments: true,
    });
    if (result.error) {
        throw new Error(`failed to initialize MSVC: ${result.error.message}`);
    }
    if (result.status !== 0) {
        const diagnostic = `${result.stdout}\n${result.stderr}`.trim().slice(-4000);
        throw new Error(`vcvarsall.bat exited with status ${result.status}: ${diagnostic}`);
    }
    return parseEnvironment(result.stdout);
}

function exportEnvironment(changes) {
    const environmentFile = process.env.GITHUB_ENV;
    if (!environmentFile) {
        throw new Error("GITHUB_ENV is unavailable");
    }
    const content = changes.map(({ name, value }) => `${name}=${value}\n`).join("");
    fs.appendFileSync(environmentFile, content, { encoding: "utf8" });
}

function main() {
    if (process.platform !== "win32") {
        throw new Error("the GDPP MSVC setup action can only run on Windows");
    }
    const architecture = resolveArchitecture(process.env.INPUT_ARCH);
    const { installation, vcvars } = findVisualStudio();
    const environment = captureEnvironment(vcvars, architecture);
    const changes = changedEnvironment(process.env, environment);
    const configuredArchitecture = environment.get("VSCMD_ARG_TGT_ARCH")?.value.toLowerCase();
    if (configuredArchitecture !== architecture) {
        throw new Error(
            `vcvarsall.bat configured '${configuredArchitecture || "unknown"}' instead of ` +
                `'${architecture}'`,
        );
    }
    if (!environment.get("INCLUDE")?.value) {
        throw new Error("vcvarsall.bat did not configure the MSVC include path");
    }
    if (!environment.get("VCTOOLSINSTALLDIR")?.value) {
        throw new Error("vcvarsall.bat did not report the selected MSVC toolset");
    }
    exportEnvironment(changes);
    process.stdout.write(
        `Configured MSVC ${architecture} from ${installation}; exported ${changes.length} variables.\n`,
    );
}

if (require.main === module) {
    try {
        main();
    } catch (error) {
        fail(error instanceof Error ? error.message : String(error));
    }
}

module.exports = {
    captureEnvironment,
    changedEnvironment,
    deduplicatePath,
    parseEnvironment,
    resolveArchitecture,
};
