const std = @import("std");

// Zig's `c++` driver does not schedule C++ modules. This build owns the
// module graph instead:
//
//   module_spec table ──► layering check (comptime)
//          │
//          ▼
//   per module: .cppm ──precompile──► .pcm (BMI, consumed by importers)
//                    └──compile─────► .o   (linked into libavionix)
//
// Every compile goes through `zig c++` so libc++ configuration, target
// flags, and sanitizer settings match between BMIs and objects. `.cppm`
// files are passed with `-x c++` because the Zig driver only applies its
// libc++ setup to extensions it recognizes.

const layer = enum(u8) {
    entity = 0,
    object = 1,
    task = 2,
    interface = 3,
    lib = 4,
    consumer = 5,
};

const module_spec = struct {
    name: []const u8,
    path: []const u8,
    imports: []const []const u8 = &.{},
};

const library_modules = [_]module_spec{
    // entity
    .{ .name = "avionix.entity.geometry", .path = "core/entity/geometry.cppm" },
    .{ .name = "avionix.entity.color", .path = "core/entity/color.cppm" },
    .{ .name = "avionix.entity.style", .path = "core/entity/style.cppm", .imports = &.{"avionix.entity.color"} },
    .{ .name = "avionix.entity.unicode", .path = "core/entity/unicode.cppm" },
    .{ .name = "avionix.entity.cell", .path = "core/entity/cell.cppm", .imports = &.{ "avionix.entity.style", "avionix.entity.unicode" } },
    .{ .name = "avionix.entity.event", .path = "core/entity/event.cppm", .imports = &.{"avionix.entity.geometry"} },
    .{ .name = "avionix.entity.constraint", .path = "core/entity/constraint.cppm", .imports = &.{"avionix.entity.geometry"} },
    .{ .name = "avionix.entity.error", .path = "core/entity/error.cppm" },

    // object
    .{ .name = "avionix.object.terminal_capabilities", .path = "core/object/terminal_capabilities.cppm", .imports = &.{"avionix.entity.color"} },
    .{ .name = "avionix.object.terminal", .path = "core/object/terminal.cppm", .imports = &.{ "avionix.entity.geometry", "avionix.entity.error" } },
    .{ .name = "avionix.object.ansi_encoder", .path = "core/object/ansi_encoder.cppm", .imports = &.{ "avionix.entity.geometry", "avionix.entity.color", "avionix.entity.style" } },
    .{ .name = "avionix.object.buffer", .path = "core/object/buffer.cppm", .imports = &.{ "avionix.entity.geometry", "avionix.entity.style", "avionix.entity.unicode", "avionix.entity.cell" } },
    .{ .name = "avionix.object.renderer", .path = "core/object/renderer.cppm", .imports = &.{ "avionix.entity.geometry", "avionix.entity.color", "avionix.entity.style", "avionix.entity.cell", "avionix.object.buffer", "avionix.object.ansi_encoder" } },
    .{ .name = "avionix.object.input_decoder", .path = "core/object/input_decoder.cppm", .imports = &.{ "avionix.entity.geometry", "avionix.entity.unicode", "avionix.entity.event" } },
    .{ .name = "avionix.object.event_queue", .path = "core/object/event_queue.cppm", .imports = &.{"avionix.entity.event"} },
    .{ .name = "avionix.object.input_reader", .path = "core/object/input_reader.cppm", .imports = &.{ "avionix.entity.geometry", "avionix.entity.event", "avionix.entity.error", "avionix.object.terminal", "avionix.object.input_decoder", "avionix.object.event_queue" } },

    // task
    .{ .name = "avionix.task.dispatch", .path = "core/task/dispatch.cppm", .imports = &.{ "avionix.entity.event", "avionix.object.event_queue" } },
    .{ .name = "avionix.task.render", .path = "core/task/render.cppm", .imports = &.{ "avionix.entity.error", "avionix.object.buffer", "avionix.object.renderer" } },
    .{ .name = "avionix.task.resize", .path = "core/task/resize.cppm", .imports = &.{ "avionix.entity.geometry", "avionix.object.renderer" } },
    .{ .name = "avionix.task.run", .path = "core/task/run.cppm", .imports = &.{ "avionix.entity.geometry", "avionix.entity.color", "avionix.entity.event", "avionix.entity.error", "avionix.object.terminal", "avionix.object.terminal_capabilities", "avionix.object.buffer", "avionix.object.renderer", "avionix.object.event_queue", "avionix.object.input_reader", "avionix.task.dispatch", "avionix.task.render", "avionix.task.resize" } },

    // interface
    .{ .name = "avionix.interface.widget", .path = "core/interface/widget.cppm", .imports = &.{ "avionix.entity.geometry", "avionix.entity.style", "avionix.entity.unicode", "avionix.entity.cell", "avionix.entity.event", "avionix.object.buffer" } },
    .{ .name = "avionix.interface.layout", .path = "core/interface/layout.cppm", .imports = &.{ "avionix.entity.geometry", "avionix.entity.event", "avionix.entity.constraint", "avionix.interface.widget" } },
    .{ .name = "avionix.interface.controls", .path = "core/interface/controls.cppm", .imports = &.{ "avionix.entity.geometry", "avionix.entity.color", "avionix.entity.style", "avionix.entity.unicode", "avionix.entity.event", "avionix.interface.widget" } },
    .{ .name = "avionix.interface.application", .path = "core/interface/application.cppm", .imports = &.{ "avionix.entity.geometry", "avionix.entity.color", "avionix.entity.style", "avionix.entity.cell", "avionix.entity.event", "avionix.entity.error", "avionix.object.buffer", "avionix.object.event_queue", "avionix.task.run", "avionix.interface.widget" } },

    // public facade
    .{ .name = "avionix", .path = "lib/avionix.cppm", .imports = &.{
        "avionix.entity.geometry",
        "avionix.entity.color",
        "avionix.entity.style",
        "avionix.entity.unicode",
        "avionix.entity.event",
        "avionix.entity.constraint",
        "avionix.entity.error",
        "avionix.entity.cell",
        "avionix.object.buffer",
        "avionix.interface.widget",
        "avionix.interface.layout",
        "avionix.interface.controls",
        "avionix.interface.application",
    } },
};

// Test support modules may import internal modules. They are never linked
// into libavionix.
const test_modules = [_]module_spec{
    .{ .name = "avionix_tests.check", .path = "tests/check.cppm" },
};

const program_spec = struct {
    name: []const u8,
    sources: []const []const u8,
    imports: []const []const u8,
    description: []const u8,
};

// Tests and benchmarks exercise internal modules directly.
const all_internal = blk: {
    var names: [library_modules.len][]const u8 = undefined;
    for (library_modules, 0..) |spec, i| names[i] = spec.name;
    break :blk names;
};

const test_program: program_spec = .{
    .name = "avionix-tests",
    .sources = &.{
        "tests/main.cpp",
        "tests/entity_tests.cpp",
        "tests/unicode_tests.cpp",
        "tests/buffer_tests.cpp",
        "tests/renderer_tests.cpp",
        "tests/input_tests.cpp",
        "tests/event_tests.cpp",
        "tests/layout_tests.cpp",
        "tests/widget_tests.cpp",
        "tests/feature_tests.cpp",
        "tests/terminal_tests.cpp",
    },
    .imports = &(all_internal ++ [_][]const u8{"avionix_tests.check"}),
    .description = "Run the Avionix test suite",
};

const benchmark_program: program_spec = .{
    .name = "avionix-benchmarks",
    .sources = &.{"benchmarks/main.cpp"},
    .imports = &all_internal,
    .description = "Run the hot-path benchmarks",
};

// Examples use only the public facade, the way an external consumer would.
const example_programs = [_]program_spec{
    .{ .name = "hello", .sources = &.{"examples/hello.cpp"}, .imports = &.{"avionix"}, .description = "Run the hello example" },
    .{ .name = "counter", .sources = &.{"examples/counter.cpp"}, .imports = &.{"avionix"}, .description = "Run the counter example" },
    .{ .name = "dashboard", .sources = &.{"examples/dashboard.cpp"}, .imports = &.{"avionix"}, .description = "Run the dashboard example" },
};

fn layerOf(comptime name: []const u8) layer {
    if (std.mem.eql(u8, name, "avionix")) return .lib;
    if (std.mem.startsWith(u8, name, "avionix.entity.")) return .entity;
    if (std.mem.startsWith(u8, name, "avionix.object.")) return .object;
    if (std.mem.startsWith(u8, name, "avionix.task.")) return .task;
    if (std.mem.startsWith(u8, name, "avionix.interface.")) return .interface;
    return .consumer;
}

fn findSpec(comptime specs: []const module_spec, comptime name: []const u8) ?module_spec {
    for (specs) |spec| {
        if (std.mem.eql(u8, spec.name, name)) return spec;
    }
    return null;
}

// Rejects arrows that point up the architecture, imports of modules declared
// later in the table (the table must be topologically ordered), and
// module/path mismatches.
comptime {
    @setEvalBranchQuota(100_000);
    for (library_modules, 0..) |spec, index| {
        const own = layerOf(spec.name);
        for (spec.imports) |import_name| {
            const dep = findSpec(library_modules[0..index], import_name) orelse
                @compileError(spec.name ++ " imports " ++ import_name ++ ", which is not declared before it");
            if (@intFromEnum(layerOf(dep.name)) > @intFromEnum(own))
                @compileError("dependency direction violated: " ++ spec.name ++ " -> " ++ import_name);
        }
        if (own != .lib) {
            var expected: []const u8 = "core/";
            const tail = spec.name["avionix.".len..];
            for (tail) |c| expected = expected ++ &[_]u8{if (c == '.') '/' else c};
            expected = expected ++ ".cppm";
            if (!std.mem.eql(u8, expected, spec.path))
                @compileError(spec.name ++ " must live at " ++ expected);
        }
    }
}

const compiled_module = struct {
    name: []const u8,
    bmi: std.Build.LazyPath,
    object: std.Build.LazyPath,
    closure: []const []const u8,
};

const module_graph = struct {
    b: *std.Build,
    cxx_flags: []const []const u8,
    modules: std.StringArrayHashMapUnmanaged(compiled_module) = .empty,

    fn get(graph: *const module_graph, name: []const u8) compiled_module {
        return graph.modules.get(name) orelse std.debug.panic("unknown module {s}", .{name});
    }

    // Clang loads transitive imports by name, so every compile receives the
    // full transitive closure of its imports.
    fn closureOf(graph: *module_graph, imports: []const []const u8) []const []const u8 {
        const b = graph.b;
        var seen: std.StringArrayHashMapUnmanaged(void) = .empty;
        for (imports) |import_name| {
            const dep = graph.get(import_name);
            for (dep.closure) |n| seen.put(b.allocator, n, {}) catch @panic("OOM");
            seen.put(b.allocator, import_name, {}) catch @panic("OOM");
        }
        return seen.keys();
    }

    fn addModuleFileArgs(graph: *module_graph, run: *std.Build.Step.Run, closure: []const []const u8) void {
        for (closure) |name| {
            run.addPrefixedFileArg(graph.b.fmt("-fmodule-file={s}=", .{name}), graph.get(name).bmi);
        }
    }

    fn compiler(graph: *module_graph, label: []const u8) *std.Build.Step.Run {
        const b = graph.b;
        const run = b.addSystemCommand(&.{ b.graph.zig_exe, "c++" });
        run.setName(label);
        run.addArgs(graph.cxx_flags);
        return run;
    }

    fn addModule(graph: *module_graph, spec: module_spec) void {
        const b = graph.b;
        const closure = graph.closureOf(spec.imports);

        const precompile = graph.compiler(b.fmt("precompile {s}", .{spec.name}));
        precompile.addArgs(&.{ "-x", "c++", "-S" });
        precompile.addFileArg(b.path(spec.path));
        // `-S` rather than `-c`: for ELF and Mach-O targets the Zig driver
        // post-processes `-c` output as an object file, which a BMI is not.
        // The driver still injects `-c`, hence the unused-argument warning.
        precompile.addArgs(&.{ "-Xclang", "-emit-module-interface", "-Wno-unused-command-line-argument" });
        graph.addModuleFileArgs(precompile, closure);
        precompile.addArg("-o");
        const bmi = precompile.addOutputFileArg(b.fmt("{s}.pcm", .{spec.name}));

        const compile = graph.compiler(b.fmt("compile {s}", .{spec.name}));
        compile.addArgs(&.{ "-x", "c++", "-c" });
        compile.addFileArg(b.path(spec.path));
        graph.addModuleFileArgs(compile, closure);
        compile.addArg("-o");
        const object = compile.addOutputFileArg(b.fmt("{s}.o", .{spec.name}));

        graph.modules.put(b.allocator, spec.name, .{
            .name = spec.name,
            .bmi = bmi,
            .object = object,
            .closure = closure,
        }) catch @panic("OOM");
    }

    fn compileSource(graph: *module_graph, path: []const u8, imports: []const []const u8) std.Build.LazyPath {
        const b = graph.b;
        const compile = graph.compiler(b.fmt("compile {s}", .{path}));
        compile.addArgs(&.{ "-x", "c++", "-c" });
        compile.addFileArg(b.path(path));
        graph.addModuleFileArgs(compile, graph.closureOf(imports));
        compile.addArg("-o");
        return compile.addOutputFileArg(b.fmt("{s}.o", .{std.fs.path.stem(path)}));
    }
};

const flag_options = struct {
    werror: bool = true,
    // Include target and optimization flags. The Compile step supplies its
    // own, so sources it compiles directly get warnings only.
    codegen: bool = true,
    warnings: bool = true,
};

// Centralized compiler options for Avionix sources. Dependencies and
// consumers do not inherit the warning set.
//
// BMIs record the language and codegen options they were built with, so
// every importer of an Avionix module must use the same -std, target,
// optimization, and sanitizer flags. consumerObject() applies them.
fn cxxFlags(
    b: *std.Build,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
    options: flag_options,
) []const []const u8 {
    var flags: std.ArrayList([]const u8) = .empty;
    flags.append(b.allocator, "-std=c++2c") catch @panic("OOM");
    if (options.warnings) {
        flags.appendSlice(b.allocator, &.{
            "-Wall",
            "-Wextra",
            "-Wpedantic",
            "-Wshadow",
            "-Wconversion",
            "-Wnon-virtual-dtor",
            "-Wold-style-cast",
            "-Woverloaded-virtual",
            "-Wimplicit-fallthrough",
        }) catch @panic("OOM");
        if (options.werror) flags.append(b.allocator, "-Werror") catch @panic("OOM");
    }
    flags.append(b.allocator, "-fno-sized-deallocation") catch @panic("OOM");
    if (!options.codegen) return flags.items;

    if (!target.query.isNative()) {
        flags.appendSlice(b.allocator, &.{ "-target", target.result.zigTriple(b.allocator) catch @panic("OOM") }) catch @panic("OOM");
    }
    flags.appendSlice(b.allocator, switch (optimize) {
        // Zig enables UBSan at -O0. Trapping mode avoids a dependency on the
        // UBSan runtime, which the final link would not otherwise include
        // because the objects are produced outside the Compile step.
        .Debug => &.{ "-O0", "-g", "-fsanitize-trap=undefined" },
        .ReleaseSafe => &.{ "-O2", "-g", "-fsanitize=undefined", "-fsanitize-trap=undefined" },
        .ReleaseFast => &.{"-O3"},
        .ReleaseSmall => &.{"-Os"},
    }) catch @panic("OOM");
    return flags.items;
}

/// Compiles one consumer C++ source that uses `import avionix;` and returns
/// the object file. Use from a dependent package's build.zig:
///
///     const avionix = @import("avionix");
///     const dep = b.dependency("avionix", .{ .target = target, .optimize = optimize });
///     exe.root_module.addObjectFile(avionix.consumerObject(b, dep, b.path("src/main.cpp"), target, optimize, &.{}));
///     exe.root_module.linkLibrary(dep.artifact("avionix"));
///
/// `target` and `optimize` must match the values passed to the dependency,
/// because the Avionix BMIs are only valid for identical codegen options.
/// `extra_flags` are appended (warnings, include paths, defines).
pub fn consumerObject(
    b: *std.Build,
    avionix_dependency: *std.Build.Dependency,
    source: std.Build.LazyPath,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
    extra_flags: []const []const u8,
) std.Build.LazyPath {
    const run = b.addSystemCommand(&.{ b.graph.zig_exe, "c++" });
    run.setName("compile avionix consumer");
    run.addArgs(cxxFlags(b, target, optimize, .{ .warnings = false }));
    run.addArgs(extra_flags);
    run.addPrefixedDirectoryArg("-fprebuilt-module-path=", avionix_dependency.namedLazyPath("modules"));
    run.addArgs(&.{ "-x", "c++", "-c" });
    run.addFileArg(source);
    run.addArg("-o");
    return run.addOutputFileArg("consumer.o");
}

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const werror = b.option(bool, "werror", "Treat warnings in Avionix sources as errors (default: true)") orelse true;

    const flags = cxxFlags(b, target, optimize, .{ .werror = werror });
    const warning_flags = cxxFlags(b, target, optimize, .{ .werror = werror, .codegen = false });

    var graph: module_graph = .{ .b = b, .cxx_flags = flags };
    for (library_modules) |spec| graph.addModule(spec);
    for (test_modules) |spec| graph.addModule(spec);

    // Library artifact: every core module plus the facade.
    const avionix = b.addLibrary(.{
        .name = "avionix",
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
            .link_libcpp = true,
        }),
    });
    for (library_modules) |spec| avionix.root_module.addObjectFile(graph.get(spec.name).object);
    b.installArtifact(avionix);

    // BMIs are installed under their module names so a consumer can use
    // `-fprebuilt-module-path=<prefix>/modules` and write `import avionix;`.
    // Clang BMIs are only valid for the same compiler version and flags.
    const bmi_dir = b.addWriteFiles();
    for (library_modules) |spec| {
        _ = bmi_dir.addCopyFile(graph.get(spec.name).bmi, b.fmt("{s}.pcm", .{spec.name}));
    }
    b.getInstallStep().dependOn(&b.addInstallDirectory(.{
        .source_dir = bmi_dir.getDirectory(),
        .install_dir = .prefix,
        .install_subdir = "modules",
    }).step);
    b.addNamedLazyPath("modules", bmi_dir.getDirectory());

    // Tests.
    const tests = addProgram(b, &graph, avionix, target, optimize, test_program);
    const run_tests = b.addRunArtifact(tests);
    run_tests.has_side_effects = true;
    run_tests.addPassthruArgs();
    b.step("test", test_program.description).dependOn(&run_tests.step);
    b.step("install-tests", "Install the test executable (for running on another machine)")
        .dependOn(&b.addInstallArtifact(tests, .{}).step);

    // Benchmarks.
    const benchmarks = addProgram(b, &graph, avionix, target, optimize, benchmark_program);
    const run_benchmarks = b.addRunArtifact(benchmarks);
    run_benchmarks.has_side_effects = true;
    run_benchmarks.addPassthruArgs();
    b.step("bench", benchmark_program.description).dependOn(&run_benchmarks.step);
    b.step("install-bench", "Install the benchmark executable").dependOn(&b.addInstallArtifact(benchmarks, .{}).step);

    // Examples.
    const examples_step = b.step("examples", "Build and install all examples");
    var example_exes: [example_programs.len]*std.Build.Step.Compile = undefined;
    for (example_programs, 0..) |spec, i| {
        const exe = addProgram(b, &graph, avionix, target, optimize, spec);
        example_exes[i] = exe;
        examples_step.dependOn(&b.addInstallArtifact(exe, .{}).step);
        const run = b.addRunArtifact(exe);
        run.has_side_effects = true;
        run.stdio = .inherit;
        run.addPassthruArgs();
        b.step(b.fmt("run-{s}", .{spec.name}), spec.description).dependOn(&run.step);
    }

    // End-to-end smoke test: each example runs under a pseudo terminal, is
    // resized, receives its quit key, and must exit cleanly. The harness is
    // plain C++ against the platform PTY API.
    const smoke = b.addExecutable(.{
        .name = "avionix-pty-smoke",
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
            .link_libcpp = true,
        }),
    });
    // The Compile step picks optimization flags itself and passes driver
    // flags that trip -Wunused-command-line-argument under -Werror.
    var harness_flags: std.ArrayList([]const u8) = .empty;
    harness_flags.appendSlice(b.allocator, warning_flags) catch @panic("OOM");
    harness_flags.append(b.allocator, "-Wno-unused-command-line-argument") catch @panic("OOM");
    smoke.root_module.addCSourceFile(.{ .file = b.path("tests/pty_smoke.cpp"), .flags = harness_flags.items });
    if (target.result.os.tag == .linux) smoke.root_module.linkSystemLibrary("util", .{});
    const smoke_step = b.step("smoke", "Run the examples under a pseudo terminal");
    b.step("install-smoke", "Install the pseudo-terminal harness").dependOn(&b.addInstallArtifact(smoke, .{}).step);
    const smoke_cases = [_]struct { index: usize, expect: []const u8, keys: []const u8 }{
        .{ .index = 0, .expect = "Hello from Avionix", .keys = "q" },
        .{ .index = 1, .expect = "count: 0", .keys = "++q" },
        .{ .index = 2, .expect = "Avionix dashboard", .keys = "\x03" },
    };
    for (smoke_cases) |case| {
        const run = b.addRunArtifact(smoke);
        run.has_side_effects = true;
        run.addArtifactArg(example_exes[case.index]);
        run.addArgs(&.{ case.expect, case.keys });
        smoke_step.dependOn(&run.step);
    }
}

fn addProgram(
    b: *std.Build,
    graph: *module_graph,
    avionix: *std.Build.Step.Compile,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
    spec: program_spec,
) *std.Build.Step.Compile {
    const exe = b.addExecutable(.{
        .name = spec.name,
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
            .link_libcpp = true,
        }),
    });
    for (spec.sources) |source| exe.root_module.addObjectFile(graph.compileSource(source, spec.imports));
    for (test_modules) |test_spec| {
        for (spec.imports) |import_name| {
            if (std.mem.eql(u8, import_name, test_spec.name)) {
                exe.root_module.addObjectFile(graph.get(test_spec.name).object);
            }
        }
    }
    exe.root_module.linkLibrary(avionix);
    return exe;
}
