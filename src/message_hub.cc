
#include <sys/eventfd.h>
#include "message_hub.hpp"
#include "event_queue.hpp"

int key_to_cpu(int key, unsigned int ncpus)
{
    // TODO: we better find a good hash function or the whole
    // concurrency model goes to crap.
    return key % ncpus;
}

void message_hub_t::init(unsigned int cpu_id, unsigned int _ncpus, event_queue_t *eqs[])
{
    current_cpu = cpu_id;
    ncpus = _ncpus;
    check("Can't support so many CPUs", ncpus > MAX_CPUS);
    for(unsigned int i = 0; i < ncpus; i++) {
        pthread_spin_init(&queues[i].lock, PTHREAD_PROCESS_PRIVATE);
        // TODO: unit tests currently don't mock the event queue, but
        // pass NULL instead. Make event_queue mockable.
        if(eqs) {
            queues[i].eq = eqs[i];
        } else {
            queues[i].eq = NULL;
        }
    }
}

message_hub_t::~message_hub_t() {
    for(unsigned int i = 0; i < ncpus; i++) {
        pthread_spin_destroy(&queues[i].lock);
    }
}
    
// Collects a message for a given CPU onto a local list.
void message_hub_t::store_message(unsigned int ncpu, cpu_message_t *msg) {
    msg->prev = NULL;
    msg->next = NULL;
    assert(ncpu < ncpus);
    msg->return_cpu = current_cpu;
    queues[ncpu].msg_local_list.push_back(msg);
}

// Pushes messages collected locally global lists available to all
// CPUs.
void message_hub_t::push_messages() {
    for(unsigned int i = 0; i < ncpus; i++) {
        // Append the local list for ith cpu to that cpu's global
        // message list.
        cpu_queue_t *queue = &queues[i];
        if(!queue->msg_local_list.empty()) {
            pthread_spin_lock(&queue->lock);
            queue->msg_global_list.append_and_clear(queue->msg_local_list);
            pthread_spin_unlock(&queue->lock);
            
            // TODO: event queue isn't mockable right now, so unit
            // tests pass NULL instead. When we refactor
            // event_queue_t, we should refactor other places so that
            // we can pass a mock version instead.
            if(queue->eq) {
                // Wakey wakey eggs and bakey
                int res = eventfd_write(queue->eq->core_notify_fd, 1);
                if(res != 0) {
                    // Perhaps the fd is overflown, let's clear it and try writing again
                    eventfd_t temp;
                    res = eventfd_read(queue->eq->core_notify_fd, &temp);
                    check("Could not clear an overflown core_notify_fd", res != 0);

                    // If it doesn't work this time, we're fucked beyond recovery
                    res = eventfd_write(queue->eq->core_notify_fd, 1);
                    check("Could not send a core notification via core_notify_fd", res != 0);
                }
            }
        }

        // TODO: we should use regular mutexes on single core CPU
        // instead of spinlocks
    }
}
    
// Pulls the messages stored in global lists for a given CPU.
void message_hub_t::pull_messages(unsigned int ncpu, msg_list_t *msg_list) {
    // Store the pointers to the elements in a temporary variable,
    // and clear the list while the whole shebang is locked.
    cpu_queue_t *queue = &queues[ncpu];
    pthread_spin_lock(&queue->lock);
    if(!queue->msg_global_list.empty()) {
        *msg_list = queue->msg_global_list;
        queue->msg_global_list.clear();
    } else {
        msg_list->clear();
    }
    pthread_spin_unlock(&queue->lock);

    // TODO: we should use regular mutexes on single core CPU
    // instead of spinlocks
}
