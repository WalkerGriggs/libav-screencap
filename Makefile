CC=gcc
CXX=g++
RM=rm -f

CPPFLAGS=-g -Wall -pthread -I /usr/include/ffmpeg
LDFLAGS=-g
LDLIBS=-lavformat -lavcodec -lavutil -lxcb

SRCS=main.cpp xgrab.cpp
OBJS=$(subst .cpp,.o,$(SRCS))

all: main

main: $(OBJS)
	$(CXX) $(LDFLAGS) -o main $(OBJS) $(LDLIBS)

clean:
	$(RM) $(OBJS)

distclean: clean
	$(RM) main
