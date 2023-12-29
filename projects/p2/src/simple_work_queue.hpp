#ifndef __SIMPLE_WORK_QUEUE_HPP_
#define __SIMPLE_WORK_QUEUE_HPP_

#include<deque>
#include<pthread.h>

using namespace std;

struct survival_bag {
    struct sockaddr_storage clientAddr;
    int connFd;
    char* directory;
};

struct work_queue {
    deque<struct survival_bag*> jobs;

    
    /* add a new job to the work queue
     * and return the number of jobs in the queue */
    int add_job(struct survival_bag* survivalBag) {
        jobs.push_back(survivalBag);
        size_t len = jobs.size();
        printf("Job added\n");
        return len;
    }
    
    /* return FALSE if no job is returned
     * otherwise return TRUE and set *job to the job */
    bool remove_job(struct survival_bag **job) {
        bool success = !this->jobs.empty();
        if (success) {
            *job = this->jobs.front();
            this->jobs.pop_front();
        }
        return success;
    }

    bool isEmpty(){
        if(jobs.size()==0){
            return true;
        }
        return false;
    }
};

#endif
