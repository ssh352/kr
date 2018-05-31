#include <bookL2.hpp>

#include <sstream>
#include <string>
#include <iostream>
#include <cctype>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>

using namespace tp;
using namespace utils;
using namespace std;

typedef BookQ<ShmCircularBuffer> BookQType;
volatile bool user_stopped = false;

void sig_handler(int signo)
{
  if (signo == SIGINT) {
    printf("Received SIGINT, exiting...\n");
  }
  user_stopped = true;
}

int main(int argc, char**argv) {
    if (argc != 3) {
        printf("Usage: %s symbol type(L1|L2)\n", argv[0]);
        std::vector<std::string> l1 = plcc_getStringArr("SubL1");
        printf("L1 subscriptions: ");
        for (auto s : l1) {
        	printf(" %s ", s.c_str());
        }
        printf("\nL2 subscriptions: ");
        std::vector<std::string> l2 = plcc_getStringArr("SubL2");
        for (auto s : l2) {
        	printf(" %s ", s.c_str());
        }
        printf("\n");
        return 0;
    }
    if (signal(SIGINT, sig_handler) == SIG_ERR)
    {
            printf("\ncan't catch SIGINT\n");
            return -1;
    }
    utils::PLCC::instance("booktap");
    BookConfig bcfg(argv[1],argv[2]);
    BookQType bq(bcfg, true);
    BookQType::Reader* book_reader = bq.newReader();
    BookDepot myBook;

    //uint64_t start_tm = utils::TimeUtil::cur_time_micro();
    user_stopped = false;
    while (!user_stopped) {
        if (book_reader->getLatestUpdateAndAdvance(myBook))
        {
        	printf("%s\n", myBook.prettyPrint().c_str());
        } else {
        	usleep(100*1000);
        }

    }
    delete book_reader;
    printf("Done.\n");
    return 0;
}
