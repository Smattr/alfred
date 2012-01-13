# Examples to make alfred:
#
#  make               Download SQLite and statically link it.
#  make STATIC=0      Don't download SQLite, but link dynamically.
#  make V=1           Build verbosely.
#  make DEBUG=1       Build with GDB hooks, assertions, etc.
#

.PHONY: default
default: alfred

# Define SOURCE_ROOT if you are doing an out-of-tree build.
ifndef SOURCE_ROOT
SOURCE_ROOT := ${CURDIR}
endif

# Location of SQLite to download. Update this if you want to use a different
# version of SQLite.
SQLITE_ROOT := http://www.sqlite.org
SQLITE_FILE := sqlite-amalgamation-3070900.zip

# Source files.
SOURCES := ${SOURCE_ROOT}/main.c

# Compiler and options.
CC    := gcc
STRIP := strip

C_FLAGS  := -Wall -Wextra
LD_FLAGS :=
DEFINES  :=
INCLUDES :=

# Verbosity settings.
ifndef V
V := 0
endif
ifeq (${V},1)
Q :=
else
Q := @
endif

# Debug settings.
ifndef DEBUG
DEBUG := 0
endif
ifeq (${DEBUG},1)
C_FLAGS += -g3 -ggdb
else
C_FLAGS += -Werror -O2 -fomit-frame-pointer
DEFINES += -DNDEBUG
endif

# Static or dynamic linking. This hasn't been tested thoroughly and probably
# needs some work.
ifndef STATIC
STATIC := 1
endif
ifeq (${STATIC},1)
SOURCES += ${SOURCE_ROOT}/sqlite/sqlite3.c
LD_FLAGS += -ldl -lpthread
endif

.PHONY: alfred
alfred: ${SOURCE_ROOT}/alfred

${SOURCE_ROOT}/alfred: $(patsubst %.c,%.o,${SOURCES})
	@echo " [LD]    $@"
	${Q}${CC} ${LD_FLAGS} -o $@ $^
	@$(if $(findstring 0,${DEBUG}),echo " [STRIP] $@",)
	${Q}$(if $(findstring 0,${DEBUG}),${STRIP} $@,)

%.o: %.c
	@echo " [CC]    $@"
	${Q}${CC} ${C_FLAGS} ${DEFINES} -I ${SOURCE_ROOT}/sqlite -o $@ -c $^

#
# Local SQLite source files, if required.
#

${SOURCE_ROOT}/sqlite:
	@echo "[MKDIR] $@"
	${Q}mkdir $@

${SOURCE_ROOT}/sqlite/sqlite3.c: |${SOURCE_ROOT}/sqlite check-wget check-unzip 
	@echo " [WGET]  ${SQLITE_FILE}"
	${Q}cd ${SOURCE_ROOT}/sqlite && wget "${SQLITE_ROOT}/${SQLITE_FILE}"
	@echo " [UNZIP] ${SQLITE_FILE}"
	${Q}cd ${SOURCE_ROOT}/sqlite && unzip -j ${SQLITE_FILE} -d .
	@echo " [RM]    ${SQLITE_FILE}"
	${Q}cd ${SOURCE_ROOT}/sqlite && rm -f ${SQLITE_FILE}

#
# Checks for the existence of tools.
#

.PHONY: check-% # FIXME: Is this syntax legal?
check-%:
	@echo " [CHECK] $(subst check-,,$@)"
	${Q}which $(subst check-,,$@) >/dev/null 2>/dev/null

#
# Clean targets
#

.PHONY: clean
clean:
	@echo " [CLEAN]"
	${Q}rm -f $(patsubst %.c,%.o,${SOURCES}) alfred

.PHONY: distclean
distclean: clean
	@echo " [CLEAN] sqlite"
	${Q}rm -rf ${SOURCE_ROOT}/sqlite
