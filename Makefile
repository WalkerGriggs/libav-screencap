CC=gcc
CXX=g++
RM=rm -f

CPPFLAGS=-g -Wall -pthread -I /usr/include/ffmpeg
LDFLAGS=-g
LDLIBS=-lavformat -lavcodec -lavdevice -lavutil -lswscale

SRCS=main.cpp
OBJS=$(subst .cpp,.o,$(SRCS))

all: main

main: $(OBJS)
	$(CXX) $(LDFLAGS) -o main $(OBJS) $(LDLIBS)

clean:
	$(RM) $(OBJS)

distclean: clean
	$(RM) main
