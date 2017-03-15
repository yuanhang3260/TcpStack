#
# A simulated TCP/IP stack
#
# Hang Yuan <yuanhang3260@gmail.com>
#
MAKE=make
CC=g++ -std=c++11
CFLAGS=-Wall -Werror -O2
LFLAGS=-lssl -lcrypto -pthread
IFLAGS=-Isrc/ -Isrc/Public/

SRC_DIR=src
OBJ_DIR=lib

HYLIB_DIR=../HyLib/
HYLIB=../HyLib/libhy.a

OBJ = $(OBJ_DIR)/BaseChannel.o \
			$(OBJ_DIR)/Host.o \
      $(OBJ_DIR)/IPHeader.o \
      $(OBJ_DIR)/Packet.o \
      $(OBJ_DIR)/PacketQueue.o \
      $(OBJ_DIR)/RecvWindow.o \
      $(OBJ_DIR)/SendWindow.o \
      $(OBJ_DIR)/TcpHeader.o \
      $(OBJ_DIR)/TcpController.o \
      $(OBJ_DIR)/TcpWindow.o \

TESTOBJ = $(OBJ_DIR)/PacketQueue_test.o \
          $(OBJ_DIR)/BaseChannel_test.o \
          $(OBJ_DIR)/RecvWindow_test.o \
          $(OBJ_DIR)/SendWindow_test.o \

TESTEXE = test/PacketQueue_test.out \
          test/BaseChannel_test.out \
          test/RecvWindow_test.out \
          test/SendWindow_test.out \

all: libhy library

test: library $(TESTEXE)

libhy:
	+$(MAKE) -C $(HYLIB_DIR)

library: $(OBJ)
	ar cr libtcp.a $(OBJ)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cc
	$(CC) $(CFLAGS) $(IFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cc $(SRC_DIR)/%.h
	$(CC) $(CFLAGS) $(IFLAGS) -c $< -o $@

test/%.out: $(OBJ_DIR)/%.o library
	$(CC) $(CFLAGS) $(LFLAGS) $< libtcp.a $(HYLIB) -o $@


clean:
	rm -rf libtcp.a
	rm -rf $(OBJ_DIR)/*.o
	rm -rf test/*.out
