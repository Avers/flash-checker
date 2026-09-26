CC      ?= cc
CFLAGS  ?= -O2 -g
WARN     = -Wall -Wextra -Werror -Wshadow -Wpointer-arith -Wcast-qual -Wno-unused-parameter
DEFS     = -D_GNU_SOURCE -DFILE_OFFSET_BITS=64
INC      = -Iinclude
ALLCFLAGS = -std=c11 $(WARN) $(DEFS) $(INC) $(CFLAGS)

SRCDIRS  = src src/device src/io src/crypto src/pattern src/test src/scheduler src/stats src/report
SRC      = $(foreach d,$(SRCDIRS),$(wildcard $(d)/*.c))
MAIN_OBJ = build/src/main.o
OBJ      = $(patsubst %.c,build/%.o,$(SRC))
BIN      = build/flashcheck

TESTSRC  = $(wildcard tests/*.c)
TESTOBJ  = $(patsubst %.c,build/%.o,$(TESTSRC))
TESTBIN  = build/flashcheck-tests
LIBOBJ   = $(filter-out $(MAIN_OBJ),$(OBJ))

.PHONY: all clean test asan debug lint install

all: $(BIN)

$(BIN): $(OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(ALLCFLAGS) -o $@ $^ $(LDFLAGS)

$(TESTBIN): $(LIBOBJ) $(TESTOBJ)
	@mkdir -p $(dir $@)
	$(CC) $(ALLCFLAGS) -o $@ $^ $(LDFLAGS)

build/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(ALLCFLAGS) -MMD -MP -c -o $@ $<

-include $(OBJ:.o=.d) $(TESTOBJ:.o=.d)

test: $(TESTBIN) $(BIN)
	@$(TESTBIN)
	@echo "== CLI smoke =="
	@./$(BIN) --version
	@./$(BIN) --help >/dev/null
	@./$(BIN) --self-test=alias; test $$? -eq 1
	@./$(BIN) --self-test=honest; test $$? -eq 0
	@./$(BIN) --self-test=stale; test $$? -eq 1
	@./$(BIN) --self-test=error; test $$? -eq 4
	@echo "== all CLI smoke tests passed =="

debug: CFLAGS = -O0 -g3 -fsanitize=address,undefined -fno-omit-frame-pointer
debug: clean $(BIN) $(TESTBIN)

asan: CFLAGS = -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
asan: clean
	@$(MAKE) --no-print-directory test

lint:
	@command -v clang-tidy >/dev/null 2>&1 && clang-tidy --version || echo "clang-tidy not installed; relying on -Werror build"
	@$(MAKE) --no-print-directory all

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)/usr/local/bin/flashcheck

clean:
	rm -rf build
