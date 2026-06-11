#!/usr/bin/env bash
set -euo pipefail
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH
: "${SANITIZER_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g}"
# Debug-info contract (SPEC §6.2 item 10): fuzz/standalone binaries must carry DWARF <= 3 so Mayhem's
# triage can read symbols. clang-19's plain -g emits DWARF-5, so thread $DEBUG_FLAGS after
# $SANITIZER_FLAGS on every compile that feeds a target/standalone binary (later -gdwarf-3 wins).
: "${DEBUG_FLAGS:=-gdwarf-3}"
: "${CC:=clang}" ; : "${CXX:=clang++}" ; : "${LIB_FUZZING_ENGINE:=-fsanitize=fuzzer}"
: "${MAYHEM_JOBS:=$(nproc)}"
export SANITIZER_FLAGS DEBUG_FLAGS CC CXX LIB_FUZZING_ENGINE MAYHEM_JOBS

cd "$SRC"
git submodule update --init --recursive

WRAPPED_SYSCALLS="-Wl,--wrap=getpeername,--wrap=sendto,--wrap=send,--wrap=recv,--wrap=read,--wrap=listen,--wrap=getaddrinfo,--wrap=freeaddrinfo,--wrap=setsockopt,--wrap=fcntl,--wrap=bind,--wrap=socket,--wrap=epoll_wait,--wrap=epoll_create1,--wrap=timerfd_settime,--wrap=close,--wrap=accept4,--wrap=eventfd,--wrap=timerfd_create,--wrap=epoll_ctl,--wrap=shutdown"

# OSS-Fuzz fuzzing/Makefile pattern — EpollEchoServer only (the fork's Mayhem target).
cd fuzzing
rm -rf *.o
"$CC" -DLIBUS_NO_SSL $SANITIZER_FLAGS $DEBUG_FLAGS -std=c11 -I../uSockets/src -O3 \
  -c ../uSockets/src/*.c ../uSockets/src/eventing/*.c ../uSockets/src/crypto/*.c
cp EpollEchoServer.dict /mayhem/
"$CXX" -DLIBUS_NO_SSL $SANITIZER_FLAGS $DEBUG_FLAGS $WRAPPED_SYSCALLS -std=c++17 -O3 -DUWS_MOCK_ZLIB \
  -I../src -I../uSockets/src EpollEchoServer.cpp \
  -o /mayhem/EpollEchoServer $LIB_FUZZING_ENGINE *.o
rm -f EpollEchoServer.o

# Standalone (non-fuzzer) reproducer.
"$CC" $SANITIZER_FLAGS $DEBUG_FLAGS -c "$STANDALONE_FUZZ_MAIN" -o /tmp/standalone_main.o
"$CXX" $SANITIZER_FLAGS $DEBUG_FLAGS $WRAPPED_SYSCALLS -std=c++17 -O3 -DUWS_MOCK_ZLIB \
  -I../src -I../uSockets/src EpollEchoServer.cpp /tmp/standalone_main.o \
  *.o -o /mayhem/EpollEchoServer-standalone
chmod +x /mayhem/EpollEchoServer /mayhem/EpollEchoServer-standalone

# Functional test suite (normal flags — no sanitizers).
cd "$SRC/tests"
mkdir -p /mayhem/tests-bin
TEST_CXXFLAGS="-std=c++17 -O2 -g"
"$CXX" $TEST_CXXFLAGS Query.cpp -o /mayhem/tests-bin/Query
"$CXX" $TEST_CXXFLAGS ChunkedEncoding.cpp -o /mayhem/tests-bin/ChunkedEncoding
"$CXX" $TEST_CXXFLAGS TopicTree.cpp -o /mayhem/tests-bin/TopicTree
"$CXX" -D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_DEBUG $TEST_CXXFLAGS HttpRouter.cpp -o /mayhem/tests-bin/HttpRouter
"$CXX" $TEST_CXXFLAGS BloomFilter.cpp -o /mayhem/tests-bin/BloomFilter
"$CXX" $TEST_CXXFLAGS ExtensionsNegotiator.cpp -o /mayhem/tests-bin/ExtensionsNegotiator
"$CXX" $TEST_CXXFLAGS HttpParser.cpp -o /mayhem/tests-bin/HttpParser
