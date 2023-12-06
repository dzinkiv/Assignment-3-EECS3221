all:
	cc New_Alarm_Cond.c -D_POSIX_PTHREAD_SEMANTICS -lpthread -lm
	./a.out