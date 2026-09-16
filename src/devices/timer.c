#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include "devices/pit.h"
#include "threads/interrupt.h"
#include "threads/synch.h"
#include "threads/thread.h"
  
/* See [8254] for hardware details of the 8254 timer chip. */

#if TIMER_FREQ < 19
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

/* Number of timer ticks since OS booted. */
static int64_t ticks;

/* Number of loops per timer tick.
   Initialized by timer_calibrate(). */
static unsigned loops_per_tick;

static intr_handler_func timer_interrupt;
static bool too_many_loops (unsigned loops);
static void busy_wait (int64_t loops);
static void real_time_sleep (int64_t num, int32_t denom);
static void real_time_delay (int64_t num, int32_t denom);

/* Uma thread dormindo em timer_sleep(), esperando para ser acordada
   quando timer_ticks() alcançar WAKE_TICK.  Cada chamador de
   timer_sleep() mantém um destes na própria pilha e o insere em
   SLEEP_LIST; o handler de interrupção do timer acorda as threads
   removendo entradas prontas do início dessa lista e sinalizando
   SEMA, que é o que de fato bloqueia/desbloqueia a thread. */
struct sleeping_thread
  {
    int64_t wake_tick;            /* Tick em que deve acordar. */
    int priority;                 /* Prioridade no momento em que
                                      timer_sleep() foi chamada, usada
                                      para desempatar threads que
                                      compartilham o mesmo WAKE_TICK. */
    struct semaphore sema;        /* Sinalizado para acordar a thread. */
    struct list_elem elem;        /* Elemento de lista para SLEEP_LIST. */
  };

/* Lista de structs sleeping_thread, ordenada de forma não decrescente
   por WAKE_TICK (empates desempatados por PRIORITY decrescente, para
   que entre threads que acordam no mesmo tick a de maior prioridade
   seja desbloqueada, e portanto escalonada, primeiro).  Protegida
   desabilitando interrupções: é acessada tanto por threads do kernel
   (em timer_sleep()) quanto pelo handler de interrupção do timer. */
static struct list sleep_list;

static bool wake_tick_less (const struct list_elem *a,
                             const struct list_elem *b, void *aux);

/* Sets up the timer to interrupt TIMER_FREQ times per second,
   and registers the corresponding interrupt. */
void
timer_init (void)
{
  list_init (&sleep_list);
  pit_configure_channel (0, 2, TIMER_FREQ);
  intr_register_ext (0x20, timer_interrupt, "8254 Timer");
}

/* Calibrates loops_per_tick, used to implement brief delays. */
void
timer_calibrate (void) 
{
  unsigned high_bit, test_bit;

  ASSERT (intr_get_level () == INTR_ON);
  printf ("Calibrating timer...  ");

  /* Approximate loops_per_tick as the largest power-of-two
     still less than one timer tick. */
  loops_per_tick = 1u << 10;
  while (!too_many_loops (loops_per_tick << 1)) 
    {
      loops_per_tick <<= 1;
      ASSERT (loops_per_tick != 0);
    }

  /* Refine the next 8 bits of loops_per_tick. */
  high_bit = loops_per_tick;
  for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)
    if (!too_many_loops (high_bit | test_bit))
      loops_per_tick |= test_bit;

  printf ("%'"PRIu64" loops/s.\n", (uint64_t) loops_per_tick * TIMER_FREQ);
}

/* Returns the number of timer ticks since the OS booted. */
int64_t
timer_ticks (void) 
{
  enum intr_level old_level = intr_disable ();
  int64_t t = ticks;
  intr_set_level (old_level);
  return t;
}

/* Returns the number of timer ticks elapsed since THEN, which
   should be a value once returned by timer_ticks(). */
int64_t
timer_elapsed (int64_t then) 
{
  return timer_ticks () - then;
}

/* Sleeps for approximately TICKS timer ticks.  Interrupts must
   be turned on. */
void
timer_sleep (int64_t ticks)
{
  struct sleeping_thread st;
  enum intr_level old_level;

  ASSERT (intr_get_level () == INTR_ON);

  if (ticks <= 0)
    return;

  st.wake_tick = timer_ticks () + ticks;
  st.priority = thread_get_priority ();
  sema_init (&st.sema, 0);

  /* Registra o pedido de despertar desta thread e bloqueia em
     ST.SEMA em vez de ficar girando.  O handler de interrupção do
     timer a acorda chamando sema_up() quando WAKE_TICK é alcançado
     (ver timer_interrupt()).  SLEEP_LIST é compartilhada com o
     handler de interrupção, então só pode ser modificada com
     interrupções desabilitadas. */
  old_level = intr_disable ();
  list_insert_ordered (&sleep_list, &st.elem, wake_tick_less, NULL);
  intr_set_level (old_level);

  sema_down (&st.sema);
}

/* Sleeps for approximately MS milliseconds.  Interrupts must be
   turned on. */
void
timer_msleep (int64_t ms) 
{
  real_time_sleep (ms, 1000);
}

/* Sleeps for approximately US microseconds.  Interrupts must be
   turned on. */
void
timer_usleep (int64_t us) 
{
  real_time_sleep (us, 1000 * 1000);
}

/* Sleeps for approximately NS nanoseconds.  Interrupts must be
   turned on. */
void
timer_nsleep (int64_t ns) 
{
  real_time_sleep (ns, 1000 * 1000 * 1000);
}

/* Busy-waits for approximately MS milliseconds.  Interrupts need
   not be turned on.

   Busy waiting wastes CPU cycles, and busy waiting with
   interrupts off for the interval between timer ticks or longer
   will cause timer ticks to be lost.  Thus, use timer_msleep()
   instead if interrupts are enabled. */
void
timer_mdelay (int64_t ms) 
{
  real_time_delay (ms, 1000);
}

/* Sleeps for approximately US microseconds.  Interrupts need not
   be turned on.

   Busy waiting wastes CPU cycles, and busy waiting with
   interrupts off for the interval between timer ticks or longer
   will cause timer ticks to be lost.  Thus, use timer_usleep()
   instead if interrupts are enabled. */
void
timer_udelay (int64_t us) 
{
  real_time_delay (us, 1000 * 1000);
}

/* Sleeps execution for approximately NS nanoseconds.  Interrupts
   need not be turned on.

   Busy waiting wastes CPU cycles, and busy waiting with
   interrupts off for the interval between timer ticks or longer
   will cause timer ticks to be lost.  Thus, use timer_nsleep()
   instead if interrupts are enabled.*/
void
timer_ndelay (int64_t ns) 
{
  real_time_delay (ns, 1000 * 1000 * 1000);
}

/* Prints timer statistics. */
void
timer_print_stats (void) 
{
  printf ("Timer: %"PRId64" ticks\n", timer_ticks ());
}

/* Timer interrupt handler. */
static void
timer_interrupt (struct intr_frame *args UNUSED)
{
  ticks++;
  thread_tick ();

  /* Acorda toda thread cujo WAKE_TICK já foi alcançado.  SLEEP_LIST
     é mantida ordenada por WAKE_TICK, então basta remover entradas
     do início até encontrar uma que ainda não está pronta.
     sema_up() pode ser chamada com segurança de um handler de
     interrupção. */
  while (!list_empty (&sleep_list))
    {
      struct sleeping_thread *st = list_entry (list_front (&sleep_list),
                                                struct sleeping_thread, elem);
      if (st->wake_tick > ticks)
        break;
      list_pop_front (&sleep_list);
      sema_up (&st->sema);
    }
}

/* Compara dois elementos de lista sleeping_thread A e B, primeiro
   pelo tick de despertar e, para threads que acordam no mesmo tick,
   por prioridade decrescente.  Usada para manter SLEEP_LIST ordenada
   de forma que timer_interrupt() acorde as threads na ordem em que
   devem rodar. */
static bool
wake_tick_less (const struct list_elem *a, const struct list_elem *b,
                void *aux UNUSED)
{
  const struct sleeping_thread *sa = list_entry (a, struct sleeping_thread,
                                                  elem);
  const struct sleeping_thread *sb = list_entry (b, struct sleeping_thread,
                                                  elem);
  if (sa->wake_tick != sb->wake_tick)
    return sa->wake_tick < sb->wake_tick;
  return sa->priority > sb->priority;
}

/* Returns true if LOOPS iterations waits for more than one timer
   tick, otherwise false. */
static bool
too_many_loops (unsigned loops) 
{
  /* Wait for a timer tick. */
  int64_t start = ticks;
  while (ticks == start)
    barrier ();

  /* Run LOOPS loops. */
  start = ticks;
  busy_wait (loops);

  /* If the tick count changed, we iterated too long. */
  barrier ();
  return start != ticks;
}

/* Iterates through a simple loop LOOPS times, for implementing
   brief delays.

   Marked NO_INLINE because code alignment can significantly
   affect timings, so that if this function was inlined
   differently in different places the results would be difficult
   to predict. */
static void NO_INLINE
busy_wait (int64_t loops) 
{
  while (loops-- > 0)
    barrier ();
}

/* Sleep for approximately NUM/DENOM seconds. */
static void
real_time_sleep (int64_t num, int32_t denom) 
{
  /* Convert NUM/DENOM seconds into timer ticks, rounding down.
          
        (NUM / DENOM) s          
     ---------------------- = NUM * TIMER_FREQ / DENOM ticks. 
     1 s / TIMER_FREQ ticks
  */
  int64_t ticks = num * TIMER_FREQ / denom;

  ASSERT (intr_get_level () == INTR_ON);
  if (ticks > 0)
    {
      /* We're waiting for at least one full timer tick.  Use
         timer_sleep() because it will yield the CPU to other
         processes. */                
      timer_sleep (ticks); 
    }
  else 
    {
      /* Otherwise, use a busy-wait loop for more accurate
         sub-tick timing. */
      real_time_delay (num, denom); 
    }
}

/* Busy-wait for approximately NUM/DENOM seconds. */
static void
real_time_delay (int64_t num, int32_t denom)
{
  /* Scale the numerator and denominator down by 1000 to avoid
     the possibility of overflow. */
  ASSERT (denom % 1000 == 0);
  busy_wait (loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000)); 
}
