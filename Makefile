# $Id$

OBJS = main.o
TARGET = battlesprak
LIBS = -lm
CFLAGS = -Wall

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LIBS) $(OBJS) -o $@

.c.o:
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f $(TARGET) $(OBJS)
