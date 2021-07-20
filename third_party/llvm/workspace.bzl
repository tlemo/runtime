"""Provides the repository macro to import LLVM."""

load("//third_party:repo.bzl", "tfrt_http_archive")

def repo(name):
    """Imports LLVM."""
    LLVM_COMMIT = "edfdb5fcd10779905ef960c8d4aa61d46b57f451"
    LLVM_SHA256 = "cf3ba31b77e869d58637c786179c05b3c293e980be601e307a0ad4f8301d5616"

    tfrt_http_archive(
        name = name,
        sha256 = LLVM_SHA256,
        strip_prefix = "llvm-project-" + LLVM_COMMIT,
        urls = [
            "https://storage.googleapis.com/mirror.tensorflow.org/github.com/llvm/llvm-project/archive/{commit}.tar.gz".format(commit = LLVM_COMMIT),
            "https://github.com/llvm/llvm-project/archive/{commit}.tar.gz".format(commit = LLVM_COMMIT),
        ],
        link_files = {
            "//third_party/llvm:llvm.autogenerated.BUILD": "llvm/BUILD",
            "//third_party/mlir:BUILD": "mlir/BUILD",
            "//third_party/mlir:build_defs.bzl": "mlir/build_defs.bzl",
            "//third_party/mlir:linalggen.bzl": "mlir/linalggen.bzl",
            "//third_party/mlir:tblgen.bzl": "mlir/tblgen.bzl",
            "//third_party/mlir:test.BUILD": "mlir/test/BUILD",
        },
    )
