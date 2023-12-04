#include "../src/types.h"
#include "../src/stat.h"
#include "../src/user.h"
#include "../src/pstat.h"

int
main(int argc, char *argv[])
{
    struct pstat st;

    if(argc != 2){
        printf(1, "usage: mytest counter");
        exit();
    }

    int i, x, l, j;
    int mypid = getpid();

    for(i = 1; i < atoi(argv[1]); i++){
        x = x + i;
    }

    getpinfo(&st);
    for (j = 0; j < NPROC; j++) {
        if (st.inuse[j] && st.pid[j] >= 3 && st.pid[j] == mypid) {
                printf(1, "pid:%d \t ticks-used:%d\n", mypid, st.ticks[j]);
        }
    }
    
    exit();
    return 0;
}
