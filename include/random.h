#ifndef RANDOM_H
#define RANDOM_H
#include "types.h"

/* Kernel CSPRNG: a hash DRBG (SHA-256) fed by an entropy pool. Sources: counter jitter at interrupts, UART/network/disk event
   timing, virtio-rng when the VM has one, and a seed file kept in /var/lib/random.seed across boots. */
void random_init();                              /* early boot: hardware sources */
void random_add_event();                         /* cheap, IRQ-safe: mixes the cycle counter (call from interrupt/IO paths) */
void random_add_bytes(const void* data, uint32_t n, uint32_t entropy_bits);
void random_get(uint8_t* out, uint32_t n);       /* never blocks; check random_ready() for key generation */
int random_ready();                              /* at least 128 bits of estimated entropy collected */
void random_seed_load();                         /* after the filesystem is up: mix the stored seed, then replace it */
int random_seed_save();
struct RandomStatus { uint32_t entropy_bits; uint32_t events; int ready; int hw_rng; int seed_loaded; uint64_t bytes_out; };
void random_status(RandomStatus* out);

int virtio_rng_init();                           /* 1 when a virtio-rng device was found */
int virtio_rng_read(uint8_t* out, uint32_t n);   /* bytes obtained (0 on failure) */
#endif
