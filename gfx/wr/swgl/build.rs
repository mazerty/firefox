/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

extern crate cc;
extern crate glsl_to_cxx;
extern crate webrender_build;

use std::collections::HashSet;
use std::fmt::Write;
use webrender_build::shader::{get_shader_features, ShaderFeatureFlags};

// Shader key is in "name feature,feature" format.
// File name needs to be formatted as "name_feature_feature".
fn shader_file(shader_key: &str) -> String {
    shader_key.replace([' ', ','], "_")
}

fn write_load_shader(shader_keys: &[String]) {
    let mut load_shader = String::new();
    for s in shader_keys {
        let _ = writeln!(load_shader, "#include \"{}.h\"", shader_file(s));
    }
    load_shader.push_str("ProgramLoader load_shader(const char* name) {\n");
    for s in shader_keys {
        let _ = writeln!(
            load_shader,
            "  if (!strcmp(name, \"{}\")) {{ return {}_program::loader; }}",
            s,
            shader_file(s)
        );
    }
    load_shader.push_str("  return nullptr;\n}\n");
    std::fs::write(
        std::env::var("OUT_DIR").unwrap() + "/load_shader.h",
        load_shader,
    )
    .unwrap();
}

fn process_imports(
    shader_dir: &str,
    shader: &str,
    included: &mut HashSet<String>,
    output: &mut String,
) {
    if !included.insert(shader.into()) {
        return;
    }
    println!("cargo:rerun-if-changed={shader_dir}/{shader}.glsl");
    let source = std::fs::read_to_string(format!("{shader_dir}/{shader}.glsl")).unwrap();
    for line in source.lines() {
        if let Some(imports) = line.strip_prefix("#include ") {
            let imports = imports.split(',');
            for import in imports {
                process_imports(shader_dir, import, included, output);
            }
        } else if line.starts_with("#version ") || line.starts_with("#extension ") {
            // ignore
        } else {
            output.push_str(line);
            output.push('\n');
        }
    }
}

fn translate_shader(
    shader_key: &str,
    shader_dir: &str,
    suppressed_env_vars: &mut Option<Vec<EnvVarGuard>>,
) {
    let mut imported = String::from("#define SWGL 1\n#define __VERSION__ 150\n");
    let _ = writeln!(
        imported,
        "#define WR_MAX_VERTEX_TEXTURE_WIDTH {}U",
        webrender_build::MAX_VERTEX_TEXTURE_WIDTH
    );

    let (basename, features) =
        shader_key.split_at(shader_key.find(' ').unwrap_or(shader_key.len()));
    if !features.is_empty() {
        for feature in features.trim().split(',') {
            let _ = writeln!(imported, "#define WR_FEATURE_{feature}");
        }
    }

    process_imports(shader_dir, basename, &mut HashSet::new(), &mut imported);

    let shader = shader_file(shader_key);

    let out_dir = std::env::var("OUT_DIR").unwrap();
    let imp_name = format!("{out_dir}/{shader}.c");
    std::fs::write(&imp_name, imported).unwrap();

    // We need to ensure that the C preprocessor does not pull compiler flags from the host or
    // target environment. Set all `CFLAGS` or `CXXFLAGS` env. vars. to empty to work around this.
    let _ = suppressed_env_vars.get_or_insert_with(|| {
        cflags_env_vars()
            .map(|(key, value)| {
                std::env::set_var(&key, "");
                EnvVarGuard {
                    key,
                    old_value: Some(value),
                }
            })
            .collect::<Vec<_>>()
    });

    let mut build = cc::Build::new();
    build.no_default_flags(true);
    if let Ok(tool) = build.try_get_compiler() {
        if tool.is_like_msvc() {
            build.flag("/EP");
            if tool.path().to_str().is_some_and(|p| p.contains("clang")) {
                build.flag("/clang:-undef");
            } else {
                build.flag("/u");
            }
        } else {
            build.flag("-xc").flag("-P").flag("-undef");
        }
    }
    build.file(&imp_name);
    let vs = build.clone().define("WR_VERTEX_SHADER", Some("1")).expand();
    let fs = build
        .clone()
        .define("WR_FRAGMENT_SHADER", Some("1"))
        .expand();
    let vs_name = format!("{out_dir}/{shader}.vert");
    let fs_name = format!("{out_dir}/{shader}.frag");
    std::fs::write(&vs_name, vs).unwrap();
    std::fs::write(&fs_name, fs).unwrap();

    let args = vec!["glsl_to_cxx".to_string(), vs_name, fs_name];
    let result = glsl_to_cxx::translate(&mut args.into_iter());
    std::fs::write(format!("{out_dir}/{shader}.h"), result).unwrap();
}

fn main() {
    let shader_dir = std::env::var("CARGO_MANIFEST_DIR").unwrap() + "/../webrender/res";

    let shader_flags = ShaderFeatureFlags::GL
        | ShaderFeatureFlags::DUAL_SOURCE_BLENDING
        | ShaderFeatureFlags::ADVANCED_BLEND_EQUATION
        | ShaderFeatureFlags::DEBUG;
    let mut shaders: Vec<String> = Vec::new();
    for (name, features) in get_shader_features(shader_flags) {
        shaders.extend(features.iter().map(|f| {
            if f.is_empty() {
                name.to_owned()
            } else {
                format!("{name} {f}")
            }
        }));
    }

    shaders.sort();

    let mut suppressed_env_vars = None;
    for shader in &shaders {
        translate_shader(shader, &shader_dir, &mut suppressed_env_vars);
    }
    drop(suppressed_env_vars); // Restore env. vars. for further compilation.

    write_load_shader(&shaders);

    println!("cargo:rerun-if-changed=src/blend.h");
    println!("cargo:rerun-if-changed=src/composite.h");
    println!("cargo:rerun-if-changed=src/gl_defs.h");
    println!("cargo:rerun-if-changed=src/glsl.h");
    println!("cargo:rerun-if-changed=src/program.h");
    println!("cargo:rerun-if-changed=src/rasterize.h");
    println!("cargo:rerun-if-changed=src/swgl_ext.h");
    println!("cargo:rerun-if-changed=src/texture.h");
    println!("cargo:rerun-if-changed=src/vector_type.h");
    println!("cargo:rerun-if-changed=src/gl.cc");
    let mut build = cc::Build::new();
    build.cpp(true);

    let _suppressed_cflags_env_vars = cflags_env_vars()
        .filter_map(|(key, value)| {
            // NOTE: Cancels out [the `moz-check` plugin added in
            // `build/moz.configure/clang_plugin.configure`'s `clang_plugin_flags`
            // assignment][crossref].
            //
            // [crossref]: https://searchfox.org/mozilla-central/rev/f008b9aa2adf2dca6bdd49855b314cb3195f6f27/build/moz.configure/clang_plugin.configure#77-80
            const MOZ_CHECK_PLUGIN_LOAD_ARGS: &str = "-Xclang -add-plugin -Xclang moz-check";

            let replaced = value
                .to_str()
                .filter(|v| v.contains(MOZ_CHECK_PLUGIN_LOAD_ARGS))?
                .replace(MOZ_CHECK_PLUGIN_LOAD_ARGS, "");

            std::env::set_var(&key, replaced);
            Some(EnvVarGuard {
                key,
                old_value: Some(value),
            })
        })
        .collect::<Vec<_>>();

    if let Ok(tool) = build.try_get_compiler() {
        if tool.is_like_msvc() {
            build
                .flag("/std:c++17")
                .flag("/EHs-")
                .flag("/GR-")
                .flag("/UMOZILLA_CONFIG_H");
        } else {
            build
                .flag("-std=c++17")
                .flag("-fno-exceptions")
                .flag("-fno-rtti")
                .flag("-fno-math-errno")
                .flag("-UMOZILLA_CONFIG_H");
        }
        // SWGL relies heavily on inlining for performance so override -Oz with -O2
        if tool.args().contains(&"-Oz".into()) {
            build.flag("-O2");
        }

        // Most GLSL compilers assume something like fast-math so we turn it on.
        // However, reciprocal division makes it so 1/1 = 0.999994 which can produce a lot of fuzz
        // in reftests and the use of reciprocal instructions usually involves a refinement step
        // which bloats our already bloated code. Further, our shader code is sufficiently parallel
        // that we're more likely to be throughput bound vs latency bound. Having fewer
        // instructions makes things easier on the processor and in places where it matters we can
        // probably explicitly use reciprocal instructions and avoid the refinement step.
        // Also, allow checks for non-finite values which fast-math may disable.
        if tool.is_like_msvc() {
            build
                .flag("/fp:fast")
                .flag("-Xclang")
                .flag("-mrecip=none")
                .flag("/clang:-fno-finite-math-only");
        } else if tool.is_like_clang() {
            // gcc only supports -mrecip=none on some targets so to keep
            // things simple we don't use -ffast-math with gcc at all
            build
                .flag("-ffast-math")
                .flag("-mrecip=none")
                .flag("-fno-finite-math-only");
        }
    }

    build
        .file("src/gl.cc")
        .define("_GLIBCXX_USE_CXX11_ABI", Some("0"))
        .include(shader_dir)
        .include("src")
        .include(std::env::var("OUT_DIR").unwrap())
        .compile("gl_cc");
}

struct EnvVarGuard {
    key: std::ffi::OsString,
    old_value: Option<std::ffi::OsString>,
}

impl Drop for EnvVarGuard {
    fn drop(&mut self) {
        let Self { key, old_value } = &*self;
        if let Some(old_value) = old_value.as_ref() {
            std::env::set_var(key, old_value);
        } else {
            std::env::remove_var(key);
        }
    }
}

fn cflags_env_vars() -> impl Iterator<Item = (std::ffi::OsString, std::ffi::OsString)> {
    std::env::vars_os().filter(|(key, _value)| {
        key.to_str().is_some_and(|key_utf8| {
            ["CFLAGS", "CXXFLAGS"]
                .iter()
                .any(|opt_name| key_utf8.contains(opt_name))
        })
    })
}
