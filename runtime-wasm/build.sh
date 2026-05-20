#!/bin/bash
#
# Build script for Motus WASM Runtime
#
# Builds a freestanding WASM module using clang directly (no Emscripten).
# The runtime has no libc dependencies - all memory functions are built-in.
#
# Requirements:
#   - LLVM/clang with wasm32 target support
#   - On macOS: brew install llvm
#

set -e

BUILD_DIR="build"
SRC_DIR="src"
CORE_DIR="../src"

mkdir -p "$BUILD_DIR"

# Find clang with WASM support
find_clang() {
    # Try homebrew LLVM first (macOS)
    if [ -x "/opt/homebrew/opt/llvm/bin/clang" ]; then
        echo "/opt/homebrew/opt/llvm/bin/clang"
        return 0
    fi
    # Try Intel homebrew path
    if [ -x "/usr/local/opt/llvm/bin/clang" ]; then
        echo "/usr/local/opt/llvm/bin/clang"
        return 0
    fi
    # Try system clang and check for wasm support
    if command -v clang &> /dev/null; then
        if clang --print-targets 2>/dev/null | grep -q wasm32; then
            echo "clang"
            return 0
        fi
    fi
    return 1
}

# Parse arguments
TARGET="${1:-wasm}"

case "$TARGET" in
    wasm)
        echo "Building freestanding WASM module..."

        CLANG=$(find_clang) || {
            echo "Error: No clang with wasm32 support found."
            echo ""
            echo "On macOS, install LLVM with homebrew:"
            echo "  brew install llvm"
            echo ""
            echo "Then re-run this script."
            exit 1
        }

        echo "Using: $CLANG"
        $CLANG --version | head -1

        # Compile to WASM
        $CLANG \
            --target=wasm32 \
            -O2 \
            -flto \
            -ffunction-sections \
            -fdata-sections \
            -nostdlib \
            -DMOT_WASM_FREESTANDING \
            -I"$CORE_DIR" \
            -I"$SRC_DIR/freestanding" \
            -Wl,--no-entry \
            -Wl,--export-dynamic \
            -Wl,--export-memory \
            -Wl,--gc-sections \
            -Wl,--allow-undefined \
            -Wl,--lto-O2 \
            -o "$BUILD_DIR/mot-runtime.wasm" \
            "$SRC_DIR/wasm_vm.c" \
            "$CORE_DIR/runtime/vm.c" \
            "$CORE_DIR/compiler/bytecode.c" \
            "$CORE_DIR/compiler/compiler.c" \
            "$CORE_DIR/compiler/partial_eval.c" \
            "$CORE_DIR/lexer/lexer.c" \
            "$CORE_DIR/parser/parser.c" \
            "$CORE_DIR/parser/ast.c" \
            "$CORE_DIR/analyzer/analyzer.c" \
            "$CORE_DIR/analyzer/scope.c" \
            "$CORE_DIR/analyzer/types.c" \
            "$CORE_DIR/analyzer/deps.c" \
            "$CORE_DIR/codegen/codegen.c" \
            "$CORE_DIR/codegen/css.c" \
            "$CORE_DIR/debug/sourcemap.c" \
            "$CORE_DIR/linker/bytecode_linker.c" \
            "$CORE_DIR/util/arena.c" \
            "$CORE_DIR/util/hash.c" \
            "$CORE_DIR/util/vec.c" \
            "$CORE_DIR/util/str.c" \
            "$CORE_DIR/schema/schema_reader.c" \
            "$CORE_DIR/mot.c"

        echo "Built: $BUILD_DIR/mot-runtime.wasm"
        ls -lh "$BUILD_DIR/mot-runtime.wasm"
        ;;

    wasm-debug)
        echo "Building freestanding WASM module (debug)..."

        CLANG=$(find_clang) || {
            echo "Error: No clang with wasm32 support found."
            echo "On macOS: brew install llvm"
            exit 1
        }

        echo "Using: $CLANG"

        $CLANG \
            --target=wasm32 \
            -g \
            -O0 \
            -nostdlib \
            -DMOT_WASM_FREESTANDING \
            -I"$CORE_DIR" \
            -I"$SRC_DIR/freestanding" \
            -Wl,--no-entry \
            -Wl,--export-dynamic \
            -Wl,--export-memory \
            -Wl,--allow-undefined \
            -o "$BUILD_DIR/mot-runtime.wasm" \
            "$SRC_DIR/wasm_vm.c" \
            "$CORE_DIR/runtime/vm.c" \
            "$CORE_DIR/compiler/bytecode.c" \
            "$CORE_DIR/compiler/compiler.c" \
            "$CORE_DIR/compiler/partial_eval.c" \
            "$CORE_DIR/lexer/lexer.c" \
            "$CORE_DIR/parser/parser.c" \
            "$CORE_DIR/parser/ast.c" \
            "$CORE_DIR/analyzer/analyzer.c" \
            "$CORE_DIR/analyzer/scope.c" \
            "$CORE_DIR/analyzer/types.c" \
            "$CORE_DIR/analyzer/deps.c" \
            "$CORE_DIR/codegen/codegen.c" \
            "$CORE_DIR/codegen/css.c" \
            "$CORE_DIR/debug/sourcemap.c" \
            "$CORE_DIR/linker/bytecode_linker.c" \
            "$CORE_DIR/util/arena.c" \
            "$CORE_DIR/util/hash.c" \
            "$CORE_DIR/util/vec.c" \
            "$CORE_DIR/util/str.c" \
            "$CORE_DIR/schema/schema_reader.c" \
            "$CORE_DIR/mot.c"

        echo "Built: $BUILD_DIR/mot-runtime.wasm (debug)"
        ls -lh "$BUILD_DIR/mot-runtime.wasm"
        ;;

    inspect)
        # Inspect the WASM module
        if [ ! -f "$BUILD_DIR/mot-runtime.wasm" ]; then
            echo "Error: WASM file not found. Run './build.sh wasm' first."
            exit 1
        fi

        echo "WASM module info:"
        if command -v wasm-objdump &> /dev/null; then
            echo ""
            echo "Exports:"
            wasm-objdump -x "$BUILD_DIR/mot-runtime.wasm" | grep -A100 "Export\[" | head -30
            echo ""
            echo "Imports:"
            wasm-objdump -x "$BUILD_DIR/mot-runtime.wasm" | grep -A100 "Import\[" | head -30
        else
            echo "(install wabt for detailed inspection: brew install wabt)"
            ls -lh "$BUILD_DIR/mot-runtime.wasm"
        fi
        ;;

    clean)
        echo "Cleaning build directory..."
        rm -rf "$BUILD_DIR"
        echo "Done."
        ;;

    *)
        echo "Usage: $0 [wasm|wasm-debug|inspect|clean]"
        echo ""
        echo "  wasm        Build optimized WASM module (default)"
        echo "  wasm-debug  Build WASM module with debug info"
        echo "  inspect     Show WASM module exports/imports"
        echo "  clean       Remove build artifacts"
        echo ""
        echo "Requirements:"
        echo "  macOS: brew install llvm"
        echo "  Linux: apt install clang lld (or equivalent)"
        exit 1
        ;;
esac
