"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");

const setup = require("../ci/.github/actions/setup-msvc/index.js");

assert.equal(setup.resolveArchitecture("x64"), "x64");
assert.equal(setup.resolveArchitecture("X86_64"), "x64");
assert.throws(() => setup.resolveArchitecture("arm64"), /ships Windows x64 only/u);

const parsed = setup.parseEnvironment(
    [
        "=C:=C:\\runner",
        "Path=C:\\MSVC\\bin;C:\\msvc\\BIN;C:\\Windows",
        "INCLUDE=C:\\MSVC\\include",
        "UNCHANGED=same",
        "VALUE_WITH_EQUALS=left=right",
        "",
    ].join("\r\n"),
);
assert.equal(parsed.has("=C:"), false);
assert.equal(parsed.get("VALUE_WITH_EQUALS").value, "left=right");

const changes = setup.changedEnvironment(
    {
        PATH: "C:\\Windows",
        unchanged: "same",
    },
    parsed,
);
assert.deepEqual(changes, [
    { name: "INCLUDE", value: "C:\\MSVC\\include" },
    { name: "Path", value: "C:\\MSVC\\bin;C:\\Windows" },
    { name: "VALUE_WITH_EQUALS", value: "left=right" },
]);
assert.throws(() => {
    setup.changedEnvironment(
        {},
        new Map([["BROKEN", { name: "BROKEN", value: "first\nsecond" }]]),
    );
}, /multiline value/u);

let invocation;
const captured = setup.captureEnvironment(
    "C:\\Program Files\\Microsoft Visual Studio\\18\\Enterprise\\VC\\Auxiliary\\Build\\vcvarsall.bat",
    "x64",
    (...parameters) => {
        invocation = parameters;
        return {
            error: undefined,
            status: 0,
            stdout: "VSCMD_ARG_TGT_ARCH=x64\r\nINCLUDE=C:\\MSVC\\include\r\n",
            stderr: "",
        };
    },
    "C:\\Windows\\System32\\cmd.exe",
);
assert.equal(captured.get("VSCMD_ARG_TGT_ARCH").value, "x64");
assert.deepEqual(invocation[1].slice(0, 4), ["/d", "/u", "/s", "/c"]);
assert.equal(
    invocation[1][4],
    'call "C:\\Program Files\\Microsoft Visual Studio\\18\\Enterprise\\VC\\Auxiliary\\Build\\vcvarsall.bat" x64 >nul && set',
);
assert.equal(invocation[2].windowsVerbatimArguments, true);

const root = path.resolve(__dirname, "..");
const action = fs.readFileSync(
    path.join(root, "ci/.github/actions/setup-msvc/action.yml"),
    "utf8",
);
assert.match(action, /^  using: node24$/mu);
for (const workflow of ["core.yml", "native-integration.yml", "host-components.yml"]) {
    const content = fs.readFileSync(path.join(root, "ci/.github/workflows", workflow), "utf8");
    assert.match(content, /uses: \.\/\.github\/actions\/setup-msvc/u);
    assert.doesNotMatch(content, /ilammy\/msvc-dev-cmd/u);
}

process.stdout.write("Node.js 24 MSVC setup action contract is valid.\n");
