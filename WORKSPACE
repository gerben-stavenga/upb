workspace(name = "upb")

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")
load("//bazel:workspace_deps.bzl", "upb_deps")
load("//bazel:workspace_defs.bzl", "system_python")

upb_deps()

http_archive(
    name = "lua",
    build_file = "//bazel:lua.BUILD",
    sha256 = "b9e2e4aad6789b3b63a056d442f7b39f0ecfca3ae0f1fc0ae4e9614401b69f4b",
    strip_prefix = "lua-5.2.4",
    urls = [
        "https://mirror.bazel.build/www.lua.org/ftp/lua-5.2.4.tar.gz",
        "https://www.lua.org/ftp/lua-5.2.4.tar.gz",
    ],
)

http_archive(
     name = "com_google_googletest",
     urls = ["https://github.com/google/googletest/archive/b6cd405286ed8635ece71c72f118e659f4ade3fb.zip"],  # 2019-01-07
     strip_prefix = "googletest-b6cd405286ed8635ece71c72f118e659f4ade3fb",
     sha256 = "ff7a82736e158c077e76188232eac77913a15dac0b22508c390ab3f88e6d6d86",
)

http_archive(
    name = "com_github_google_benchmark",
    urls = ["https://github.com/google/benchmark/archive/0baacde3618ca617da95375e0af13ce1baadea47.zip"],
    strip_prefix = "benchmark-0baacde3618ca617da95375e0af13ce1baadea47",
    sha256 = "62e2f2e6d8a744d67e4bbc212fcfd06647080de4253c97ad5c6749e09faf2cb0",
)

http_archive(
    name = "com_google_googleapis",
    urls = ["https://github.com/googleapis/googleapis/archive/refs/heads/master.zip"],
    build_file = "//benchmarks:BUILD.googleapis",
    strip_prefix = "googleapis-master",
    patch_cmds = ["find google -type f -name BUILD.bazel -delete"],
)

system_python(
    name = "system_python"
)

http_archive(
    name = "rules_fuzzing",
    sha256 = "127d7c45e9b7520b3c42145b3cb1b8c26477cdaed0521b02a0298907339fefa1",
    strip_prefix = "rules_fuzzing-0.2.0",
    urls = ["https://github.com/bazelbuild/rules_fuzzing/archive/v0.2.0.zip"],
)

load("@rules_fuzzing//fuzzing:repositories.bzl", "rules_fuzzing_dependencies")

rules_fuzzing_dependencies()

load("@rules_fuzzing//fuzzing:init.bzl", "rules_fuzzing_init")

rules_fuzzing_init()
