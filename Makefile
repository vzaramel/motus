CC = gcc
CFLAGS = -std=c99 -Wall -Wextra -Wpedantic -g -I$(SRC_DIR)
LDFLAGS =

SRC_DIR = src
BUILD_DIR = build
TEST_DIR = tests

# Find all library source files (exclude CLI entrypoints)
SRCS = $(shell find $(SRC_DIR) -name '*.c' ! -path '$(SRC_DIR)/cli/*')
OBJS = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS))

# Main library
LIB = $(BUILD_DIR)/libmot.a

# CLI tool
CLI = $(BUILD_DIR)/mot
TRANSPILER_CLI = $(BUILD_DIR)/mot-bytecode-transpiler

# Cap'n Proto plugin
CAPNPC_MOT = $(BUILD_DIR)/capnpc-mot
CXX = c++
CXXFLAGS = -std=c++17

.PHONY: all lib cli transpiler lsp vscode vscode-install test clean site capnpc-mot

all: lib

lib: $(LIB)

CLI_SRC = $(SRC_DIR)/cli/mot_cli.c
TRANSPILER_CLI_SRC = $(SRC_DIR)/cli/mot_bytecode_transpiler_cli.c
LSP_CLI_SRC = $(SRC_DIR)/cli/mot_lsp.c

cli: $(LIB) $(CLI_SRC)
	$(CC) $(CFLAGS) $(CLI_SRC) -L$(BUILD_DIR) -lmot -o $(CLI)

transpiler: $(LIB) $(TRANSPILER_CLI_SRC)
	$(CC) $(CFLAGS) $(TRANSPILER_CLI_SRC) -L$(BUILD_DIR) -lmot -o $(TRANSPILER_CLI)

lsp: $(LIB) $(LSP_CLI_SRC)
	$(CC) $(CFLAGS) $(LSP_CLI_SRC) -L$(BUILD_DIR) -lmot -o $(BUILD_DIR)/mot-lsp

capnpc-mot: tools/capnpc-mot.cpp
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $(CAPNPC_MOT) $< -lcapnp -lkj

# VSCode extension
VSCODE_DIR = editors/vscode
VSCODE_EXT = $(VSCODE_DIR)/motus-$(shell cat $(VSCODE_DIR)/package.json | grep '"version"' | sed 's/.*:.*"\([^"]*\)".*/\1/').vsix

.PHONY: vscode vscode-install
vscode: lsp
	@echo "Building VSCode extension..."
	@cd $(VSCODE_DIR) && \
	if command -v vsce >/dev/null 2>&1; then \
		vsce package; \
		echo "Extension built: motus-*.vsix"; \
	else \
		echo "Warning: vsce not found. Install with: npm install -g @vscode/vsce"; \
		echo "Or manually run: cd $(VSCODE_DIR) && vsce package"; \
	fi

vscode-install: lsp vscode
	@echo "Installing VSCode extension..."
	@cd $(VSCODE_DIR) && \
	if command -v code >/dev/null 2>&1; then \
		code --install-extension motus-*.vsix --force; \
		echo "Extension installed!"; \
	else \
		echo "Error: VSCode 'code' command not found"; \
		echo "Manually install with: code --install-extension $(VSCODE_DIR)/motus-*.vsix"; \
	fi

$(LIB): $(OBJS)
	@mkdir -p $(dir $@)
	ar rcs $@ $^

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Test targets
TEST_LEXER_SRCS = $(TEST_DIR)/lexer/test_lexer.c
TEST_LEXER = $(BUILD_DIR)/test_lexer

test_lexer: $(LIB) $(TEST_LEXER_SRCS)
	$(CC) $(CFLAGS) $(TEST_LEXER_SRCS) -L$(BUILD_DIR) -lmot -o $(TEST_LEXER)
	./$(TEST_LEXER)

TEST_PARSER_SRCS = $(TEST_DIR)/parser/test_parser.c
TEST_PARSER = $(BUILD_DIR)/test_parser

test_parser: $(LIB) $(TEST_PARSER_SRCS)
	$(CC) $(CFLAGS) $(TEST_PARSER_SRCS) -L$(BUILD_DIR) -lmot -o $(TEST_PARSER)
	./$(TEST_PARSER)

TEST_ANALYZER_SRCS = $(TEST_DIR)/analyzer/test_analyzer.c
TEST_ANALYZER = $(BUILD_DIR)/test_analyzer

test_analyzer: $(LIB) $(TEST_ANALYZER_SRCS)
	$(CC) $(CFLAGS) $(TEST_ANALYZER_SRCS) -L$(BUILD_DIR) -lmot -o $(TEST_ANALYZER)
	./$(TEST_ANALYZER)

TEST_COMPILER_SRCS = $(TEST_DIR)/compiler/test_compiler.c
TEST_COMPILER = $(BUILD_DIR)/test_compiler

test_compiler: $(LIB) $(TEST_COMPILER_SRCS)
	$(CC) $(CFLAGS) $(TEST_COMPILER_SRCS) -L$(BUILD_DIR) -lmot -o $(TEST_COMPILER)
	./$(TEST_COMPILER)

TEST_PARTIAL_EVAL_SRCS = $(TEST_DIR)/compiler/test_partial_eval.c
TEST_PARTIAL_EVAL = $(BUILD_DIR)/test_partial_eval

test_partial_eval: $(LIB) $(TEST_PARTIAL_EVAL_SRCS)
	$(CC) $(CFLAGS) $(TEST_PARTIAL_EVAL_SRCS) -L$(BUILD_DIR) -lmot -o $(TEST_PARTIAL_EVAL)
	./$(TEST_PARTIAL_EVAL)

TEST_VM_SRCS = $(TEST_DIR)/runtime/test_vm.c
TEST_VM = $(BUILD_DIR)/test_vm

test_vm: $(LIB) $(TEST_VM_SRCS)
	$(CC) $(CFLAGS) $(TEST_VM_SRCS) -L$(BUILD_DIR) -lmot -o $(TEST_VM)
	./$(TEST_VM)

TEST_CODEGEN_SRCS = $(TEST_DIR)/codegen/test_codegen.c
TEST_CODEGEN = $(BUILD_DIR)/test_codegen

test_codegen: $(LIB) $(TEST_CODEGEN_SRCS)
	$(CC) $(CFLAGS) $(TEST_CODEGEN_SRCS) -L$(BUILD_DIR) -lmot -o $(TEST_CODEGEN)
	./$(TEST_CODEGEN)

TEST_API_SRCS = $(TEST_DIR)/api/test_api.c
TEST_API = $(BUILD_DIR)/test_api

test_api: $(LIB) $(TEST_API_SRCS)
	$(CC) $(CFLAGS) $(TEST_API_SRCS) -L$(BUILD_DIR) -lmot -o $(TEST_API)
	./$(TEST_API)

TEST_CLI_TARGET = $(TEST_DIR)/cli/test_cli_target.sh

test_cli: cli $(TEST_CLI_TARGET)
	chmod +x $(TEST_CLI_TARGET)
	./$(TEST_CLI_TARGET)

TEST_DEBUG_METADATA_SRCS = $(TEST_DIR)/debug/test_debug_metadata.c
TEST_DEBUG_METADATA = $(BUILD_DIR)/test_debug_metadata

test_debug_metadata: $(LIB) $(TEST_DEBUG_METADATA_SRCS)
	$(CC) $(CFLAGS) $(TEST_DEBUG_METADATA_SRCS) -L$(BUILD_DIR) -lmot -o $(TEST_DEBUG_METADATA)
	./$(TEST_DEBUG_METADATA)

TEST_SOURCEMAP_SRCS = $(TEST_DIR)/debug/test_sourcemap.c
TEST_SOURCEMAP = $(BUILD_DIR)/test_sourcemap

test_sourcemap: $(LIB) $(TEST_SOURCEMAP_SRCS)
	$(CC) $(CFLAGS) $(TEST_SOURCEMAP_SRCS) -L$(BUILD_DIR) -lmot -o $(TEST_SOURCEMAP)
	./$(TEST_SOURCEMAP)

TEST_HOST_CONTRACT_SRCS = $(TEST_DIR)/host/test_contract.c
TEST_HOST_CONTRACT = $(BUILD_DIR)/test_host_contract

test_host_contract: $(LIB) $(TEST_HOST_CONTRACT_SRCS)
	$(CC) $(CFLAGS) $(TEST_HOST_CONTRACT_SRCS) -L$(BUILD_DIR) -lmot -o $(TEST_HOST_CONTRACT)
	./$(TEST_HOST_CONTRACT)

TEST_TRANSPILER_SRCS = $(TEST_DIR)/host/test_transpiler.c
TEST_TRANSPILER = $(BUILD_DIR)/test_transpiler

test_transpiler: $(LIB) $(TEST_TRANSPILER_SRCS)
	$(CC) $(CFLAGS) $(TEST_TRANSPILER_SRCS) -L$(BUILD_DIR) -lmot -o $(TEST_TRANSPILER)
	./$(TEST_TRANSPILER)

TEST_WASM_PARITY = $(TEST_DIR)/parity/test_wasm_js_parity.sh

test_wasm_parity:
	chmod +x $(TEST_WASM_PARITY)
	./$(TEST_WASM_PARITY)

TEST_INTEGRATION_SRCS = $(TEST_DIR)/integration/test_end_to_end.c
TEST_INTEGRATION = $(BUILD_DIR)/test_integration

test_integration: $(LIB) $(TEST_INTEGRATION_SRCS)
	$(CC) $(CFLAGS) $(TEST_INTEGRATION_SRCS) -L$(BUILD_DIR) -lmot -o $(TEST_INTEGRATION)
	./$(TEST_INTEGRATION)

TEST_SCHEMA_SRCS = $(TEST_DIR)/schema/test_schema.c
TEST_SCHEMA = $(BUILD_DIR)/test_schema

test_schema: $(LIB) $(TEST_SCHEMA_SRCS)
	$(CC) $(CFLAGS) $(TEST_SCHEMA_SRCS) -L$(BUILD_DIR) -lmot -o $(TEST_SCHEMA)
	./$(TEST_SCHEMA)

test: test_lexer test_parser test_analyzer test_compiler test_partial_eval test_vm test_codegen test_api test_cli test_debug_metadata test_sourcemap test_host_contract test_transpiler test_integration test_schema

# Static site generation from .mot source files
SITE_DIR = site
SITE_COMPONENTS = $(SITE_DIR)/components/Header.mot $(SITE_DIR)/components/Footer.mot
DOCS_OUT = docs

site: cli
	@mkdir -p $(DOCS_OUT)/docs $(DOCS_OUT)/playground
	@cat $(SITE_COMPONENTS) $(SITE_DIR)/pages/index.mot > $(BUILD_DIR)/_site_index.mot
	$(CLI) --input $(BUILD_DIR)/_site_index.mot --html-out $(DOCS_OUT)/index.html
	@cat $(SITE_COMPONENTS) $(SITE_DIR)/pages/docs.mot > $(BUILD_DIR)/_site_docs.mot
	$(CLI) --input $(BUILD_DIR)/_site_docs.mot --html-out $(DOCS_OUT)/docs/index.html
	@cat $(SITE_COMPONENTS) $(SITE_DIR)/pages/playground.mot > $(BUILD_DIR)/_site_playground.mot
	$(CLI) --input $(BUILD_DIR)/_site_playground.mot --html-out $(DOCS_OUT)/playground/index.html
	@rm -f $(BUILD_DIR)/_site_*.mot
	@echo "Site generated in $(DOCS_OUT)/"

clean:
	rm -rf $(BUILD_DIR)

# Debug: print variables
debug:
	@echo "SRCS: $(SRCS)"
	@echo "OBJS: $(OBJS)"
