/* Copyright JS Foundation and other contributors, http://js.foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "ecma-async-generator-object.h"
#include "ecma-function-object.h"
#include "ecma-globals.h"
#include "ecma-helpers.h"
#include "ecma-jobqueue.h"
#include "ecma-objects.h"
#include "ecma-promise-object.h"
#include "jcontext.h"
#include "opcodes.h"

#if ENABLED (JERRY_BUILTIN_PROMISE)

/**
 * Mask for job queue type.
 */
#define ECMA_JOB_QUEURE_TYPE_MASK ((uintptr_t) 0x07)

/** \addtogroup ecma ECMA
 * @{
 *
 * \addtogroup ecmajobqueue ECMA Job Queue related routines
 * @{
 */

/**
 * Description of the PromiseReactionJob
 */
typedef struct
{
  ecma_job_queue_item_t header; /**< job queue item header */
  ecma_value_t capability; /**< capability object */
  ecma_value_t handler; /**< handler function */
  ecma_value_t argument; /**< argument for the reaction */
} ecma_job_promise_reaction_t;

/**
 * Description of the PromiseAsyncReactionJob
 */
typedef struct
{
  ecma_job_queue_item_t header; /**< job queue item header */
  ecma_value_t executable_object; /**< executable object */
  ecma_value_t argument; /**< argument for the reaction */
} ecma_job_promise_async_reaction_t;

/**
 * Description of the PromiseAsyncGeneratorJob
 */
typedef struct
{
  ecma_job_queue_item_t header; /**< job queue item header */
  ecma_value_t executable_object; /**< executable object */
} ecma_job_promise_async_generator_t;

/**
 * Description of the PromiseResolveThenableJob
 */
typedef struct
{
  ecma_job_queue_item_t header; /**< job queue item header */
  ecma_value_t promise; /**< promise to be resolved */
  ecma_value_t thenable; /**< thenable object */
  ecma_value_t then; /**< 'then' function */
} ecma_job_promise_resolve_thenable_t;

/**
 * Initialize the jobqueue.
 */
void ecma_job_queue_init (void)
{
  JERRY_CONTEXT (job_queue_head_p) = NULL;
  JERRY_CONTEXT (job_queue_tail_p) = NULL;
} /* ecma_job_queue_init */

/**
 * Get the type of the job.
 *
 * @return type of the job
 */
static inline ecma_job_queue_item_type_t JERRY_ATTR_ALWAYS_INLINE
ecma_job_queue_get_type (ecma_job_queue_item_t *job_p) /**< the job */
{
  return (ecma_job_queue_item_type_t) (job_p->next_and_type & ECMA_JOB_QUEURE_TYPE_MASK);
} /* ecma_job_queue_get_type */

/**
 * Get the next job of the job queue.
 *
 * @return next job
 */
static inline ecma_job_queue_item_t *JERRY_ATTR_ALWAYS_INLINE
ecma_job_queue_get_next (ecma_job_queue_item_t *job_p) /**< the job */
{
  return (ecma_job_queue_item_t *) (job_p->next_and_type & ~ECMA_JOB_QUEURE_TYPE_MASK);
} /* ecma_job_queue_get_next */

/**
 * Free the heap and the member of the PromiseReactionJob.
 */
static void
ecma_free_promise_reaction_job (ecma_job_promise_reaction_t *job_p) /**< points to the PromiseReactionJob */
{
  JERRY_ASSERT (job_p != NULL);

  ecma_free_value (job_p->capability);
  ecma_free_value (job_p->handler);
  ecma_free_value (job_p->argument);

  jmem_heap_free_block (job_p, sizeof (ecma_job_promise_reaction_t));
} /* ecma_free_promise_reaction_job */

/**
 * Free the heap and the member of the PromiseAsyncReactionJob.
 */
static void
ecma_free_promise_async_reaction_job (ecma_job_promise_async_reaction_t *job_p) /**< points to the
                                                                                 *   PromiseAsyncReactionJob */
{
  JERRY_ASSERT (job_p != NULL);

  ecma_free_value (job_p->executable_object);
  ecma_free_value (job_p->argument);

  jmem_heap_free_block (job_p, sizeof (ecma_job_promise_async_reaction_t));
} /* ecma_free_promise_async_reaction_job */

/**
 * Free the heap and the member of the PromiseAsyncGeneratorJob.
 */
static void
ecma_free_promise_async_generator_job (ecma_job_promise_async_generator_t *job_p) /**< points to the
                                                                                   *   PromiseAsyncReactionJob */
{
  JERRY_ASSERT (job_p != NULL);

  ecma_free_value (job_p->executable_object);

  jmem_heap_free_block (job_p, sizeof (ecma_job_promise_async_generator_t));
} /* ecma_free_promise_async_generator_job */

/**
 * Free the heap and the member of the PromiseResolveThenableJob.
 */
static void
ecma_free_promise_resolve_thenable_job (ecma_job_promise_resolve_thenable_t *job_p) /**< points to the
                                                                                     *   PromiseResolveThenableJob */
{
  JERRY_ASSERT (job_p != NULL);

  ecma_free_value (job_p->promise);
  ecma_free_value (job_p->thenable);
  ecma_free_value (job_p->then);

  jmem_heap_free_block (job_p, sizeof (ecma_job_promise_resolve_thenable_t));
} /* ecma_free_promise_resolve_thenable_job */

/**
 * The processor for PromiseReactionJob.
 *
 * See also: ES2015 25.4.2.1
 *
 * @return ecma value
 *         Returned value must be freed with ecma_free_value
 */
static ecma_value_t
ecma_process_promise_reaction_job (ecma_job_promise_reaction_t *job_p) /**< the job to be operated */
{
  ecma_string_t *resolve_str_p = ecma_get_magic_string (LIT_INTERNAL_MAGIC_STRING_PROMISE_PROPERTY_RESOLVE);
  ecma_string_t *reject_str_p = ecma_get_magic_string (LIT_INTERNAL_MAGIC_STRING_PROMISE_PROPERTY_REJECT);

  /* 2. */
  ecma_value_t capability = job_p->capability;
  /* 3. */
  ecma_value_t handler = job_p->handler;

  JERRY_ASSERT (ecma_is_value_boolean (handler) || ecma_op_is_callable (handler));

  ecma_value_t handler_result;

  if (ecma_is_value_boolean (handler))
  {
    /* 4-5. True indicates "identity" and false indicates "thrower" */
    handler_result = ecma_copy_value (job_p->argument);
  }
  else
  {
    /* 6. */
    handler_result = ecma_op_function_call (ecma_get_object_from_value (handler),
                                            ECMA_VALUE_UNDEFINED,
                                            &(job_p->argument),
                                            1);
  }

  ecma_value_t status;

  if (ecma_is_value_false (handler) || ECMA_IS_VALUE_ERROR (handler_result))
  {
    if (ECMA_IS_VALUE_ERROR (handler_result))
    {
      handler_result = jcontext_take_exception ();
    }

    /* 7. */
    ecma_value_t reject = ecma_op_object_get (ecma_get_object_from_value (capability), reject_str_p);

    JERRY_ASSERT (ecma_op_is_callable (reject));

    status = ecma_op_function_call (ecma_get_object_from_value (reject),
                                    ECMA_VALUE_UNDEFINED,
                                    &handler_result,
                                    1);
    ecma_free_value (reject);
  }
  else
  {
    /* 8. */
    ecma_value_t resolve = ecma_op_object_get (ecma_get_object_from_value (capability), resolve_str_p);

    JERRY_ASSERT (ecma_op_is_callable (resolve));

    status = ecma_op_function_call (ecma_get_object_from_value (resolve),
                                    ECMA_VALUE_UNDEFINED,
                                    &handler_result,
                                    1);
    ecma_free_value (resolve);
  }

  ecma_free_value (handler_result);
  ecma_free_promise_reaction_job (job_p);

  return status;
} /* ecma_process_promise_reaction_job */

/**
 * The processor for PromiseAsyncReactionJob.
 *
 * @return ecma value
 *         Returned value must be freed with ecma_free_value
 */
static ecma_value_t
ecma_process_promise_async_reaction_job (ecma_job_promise_async_reaction_t *job_p) /**< the job to be operated */
{
  ecma_object_t *object_p = ecma_get_object_from_value (job_p->executable_object);
  vm_executable_object_t *executable_object_p = (vm_executable_object_t *) object_p;

  if (ecma_job_queue_get_type (&job_p->header) == ECMA_JOB_PROMISE_ASYNC_REACTION_REJECTED)
  {
    if (!(executable_object_p->extended_object.u.class_prop.extra_info & ECMA_GENERATOR_ITERATE_AND_YIELD))
    {
      executable_object_p->frame_ctx.byte_code_p = opfunc_resume_executable_object_with_throw;
    }
    else if (ECMA_ASYNC_YIELD_ITERATOR_GET_STATE (executable_object_p) == ECMA_ASYNC_YIELD_ITERATOR_AWAIT_RETURN)
    {
      /* Unlike other operations, return captures rejected promises as well. */
      ECMA_ASYNC_YIELD_ITERATOR_CHANGE_STATE (executable_object_p, RETURN, OPERATION);
    }
    else
    {
      /* Exception: Abort iterators, clear all status. */
      ECMA_ASYNC_YIELD_ITERATOR_END (executable_object_p);

      JERRY_ASSERT (ecma_is_value_object (executable_object_p->frame_ctx.block_result));
      executable_object_p->frame_ctx.block_result = ECMA_VALUE_UNDEFINED;
      executable_object_p->frame_ctx.byte_code_p = opfunc_resume_executable_object_with_throw;

      JERRY_ASSERT (executable_object_p->frame_ctx.stack_top_p[-1] == ECMA_VALUE_UNDEFINED
                    || ecma_is_value_object (executable_object_p->frame_ctx.stack_top_p[-1]));
      executable_object_p->frame_ctx.stack_top_p--;
    }
  }

  if (executable_object_p->extended_object.u.class_prop.extra_info & ECMA_GENERATOR_ITERATE_AND_YIELD)
  {
    job_p->argument = ecma_async_yield_continue_await (executable_object_p, job_p->argument);

    if (ECMA_IS_VALUE_ERROR (job_p->argument))
    {
      job_p->argument = jcontext_take_exception ();
      executable_object_p->frame_ctx.byte_code_p = opfunc_resume_executable_object_with_throw;
    }
    else if (executable_object_p->extended_object.u.class_prop.extra_info & ECMA_GENERATOR_ITERATE_AND_YIELD)
    {
      /* Continue iteration. */
      JERRY_ASSERT (job_p->argument == ECMA_VALUE_UNDEFINED);

      ecma_free_promise_async_reaction_job (job_p);
      return ECMA_VALUE_UNDEFINED;
    }

    /* End of yield*, clear all status. */
    ECMA_ASYNC_YIELD_ITERATOR_END (executable_object_p);

    JERRY_ASSERT (ecma_is_value_object (executable_object_p->frame_ctx.block_result));
    executable_object_p->frame_ctx.block_result = ECMA_VALUE_UNDEFINED;

    JERRY_ASSERT (executable_object_p->frame_ctx.stack_top_p[-1] == ECMA_VALUE_UNDEFINED
                  || ecma_is_value_object (executable_object_p->frame_ctx.stack_top_p[-1]));
    executable_object_p->frame_ctx.stack_top_p--;
  }

  ecma_value_t result = opfunc_resume_executable_object (executable_object_p, job_p->argument);
  /* Argument reference has been taken by opfunc_resume_executable_object. */
  job_p->argument = ECMA_VALUE_UNDEFINED;

  uint16_t expected_bits = (ECMA_EXECUTABLE_OBJECT_COMPLETED | ECMA_ASYNC_GENERATOR_CALLED);
  if ((executable_object_p->extended_object.u.class_prop.extra_info & expected_bits) == expected_bits)
  {
    ecma_async_generator_finalize (executable_object_p, result);
    result = ECMA_VALUE_UNDEFINED;
  }

  ecma_free_promise_async_reaction_job (job_p);
  return result;
} /* ecma_process_promise_async_reaction_job */

/**
 * The processor for PromiseAsyncGeneratorJob.
 */
static void
ecma_process_promise_async_generator_job (ecma_job_promise_async_generator_t *job_p) /**< the job to be operated */
{
  ecma_object_t *object_p = ecma_get_object_from_value (job_p->executable_object);

  ecma_async_generator_run ((vm_executable_object_t *) object_p);

  ecma_free_value (job_p->executable_object);
  jmem_heap_free_block (job_p, sizeof (ecma_job_promise_async_generator_t));
} /* ecma_process_promise_async_generator_job */

/**
 * Process the PromiseResolveThenableJob.
 *
 * See also: ES2015 25.4.2.2
 *
 * @return ecma value
 *         Returned value must be freed with ecma_free_value
 */
static ecma_value_t
ecma_process_promise_resolve_thenable_job (ecma_job_promise_resolve_thenable_t *job_p) /**< the job to be operated */
{
  ecma_object_t *promise_p = ecma_get_object_from_value (job_p->promise);
  ecma_promise_resolving_functions_t funcs;
  ecma_promise_create_resolving_functions (promise_p, &funcs, true);

  ecma_string_t *str_resolve_p = ecma_get_magic_string (LIT_INTERNAL_MAGIC_STRING_RESOLVE_FUNCTION);
  ecma_string_t *str_reject_p = ecma_get_magic_string (LIT_INTERNAL_MAGIC_STRING_REJECT_FUNCTION);

  ecma_op_object_put (promise_p,
                      str_resolve_p,
                      funcs.resolve,
                      false);
  ecma_op_object_put (promise_p,
                      str_reject_p,
                      funcs.reject,
                      false);

  ecma_value_t argv[] = { funcs.resolve, funcs.reject };
  ecma_value_t ret;
  ecma_value_t then_call_result = ecma_op_function_call (ecma_get_object_from_value (job_p->then),
                                                         job_p->thenable,
                                                         argv,
                                                         2);

  ret = then_call_result;

  if (ECMA_IS_VALUE_ERROR (then_call_result))
  {
    then_call_result = jcontext_take_exception ();

    ret = ecma_op_function_call (ecma_get_object_from_value (funcs.reject),
                                 ECMA_VALUE_UNDEFINED,
                                 &then_call_result,
                                 1);

    ecma_free_value (then_call_result);
  }

  ecma_promise_free_resolving_functions (&funcs);
  ecma_free_promise_resolve_thenable_job (job_p);

  return ret;
} /* ecma_process_promise_resolve_thenable_job */

/**
 * Enqueue a Promise job into the jobqueue.
 */
static void
ecma_enqueue_job (ecma_job_queue_item_t *job_p) /**< the job */
{
  JERRY_ASSERT (job_p->next_and_type <= ECMA_JOB_QUEURE_TYPE_MASK);

  if (JERRY_CONTEXT (job_queue_head_p) == NULL)
  {
    JERRY_CONTEXT (job_queue_head_p) = job_p;
    JERRY_CONTEXT (job_queue_tail_p) = job_p;
  }
  else
  {
    JERRY_ASSERT ((JERRY_CONTEXT (job_queue_tail_p)->next_and_type & ~ECMA_JOB_QUEURE_TYPE_MASK) == 0);

    JERRY_CONTEXT (job_queue_tail_p)->next_and_type |= (uintptr_t) job_p;
    JERRY_CONTEXT (job_queue_tail_p) = job_p;
  }
} /* ecma_enqueue_job */

/**
 * Enqueue a PromiseReactionJob into the job queue.
 */
void
ecma_enqueue_promise_reaction_job (ecma_value_t capability, /**< capability object */
                                   ecma_value_t handler, /**< handler function */
                                   ecma_value_t argument) /**< argument for the reaction */
{
  ecma_job_promise_reaction_t *job_p;
  job_p = (ecma_job_promise_reaction_t *) jmem_heap_alloc_block (sizeof (ecma_job_promise_reaction_t));
  job_p->header.next_and_type = ECMA_JOB_PROMISE_REACTION;
  job_p->capability = ecma_copy_value (capability);
  job_p->handler = ecma_copy_value (handler);
  job_p->argument = ecma_copy_value (argument);

  ecma_enqueue_job (&job_p->header);
} /* ecma_enqueue_promise_reaction_job */

/**
 * Enqueue a PromiseAsyncReactionJob into the job queue.
 */
void
ecma_enqueue_promise_async_reaction_job (ecma_value_t executable_object, /**< executable object */
                                         ecma_value_t argument, /**< argument */
                                         bool is_rejected) /**< is_fulfilled */
{
  ecma_job_promise_async_reaction_t *job_p;
  job_p = (ecma_job_promise_async_reaction_t *) jmem_heap_alloc_block (sizeof (ecma_job_promise_async_reaction_t));
  job_p->header.next_and_type = (is_rejected ? ECMA_JOB_PROMISE_ASYNC_REACTION_REJECTED
                                             : ECMA_JOB_PROMISE_ASYNC_REACTION_FULFILLED);
  job_p->executable_object = ecma_copy_value (executable_object);
  job_p->argument = ecma_copy_value (argument);

  ecma_enqueue_job (&job_p->header);
} /* ecma_enqueue_promise_async_reaction_job */

/**
 * Enqueue a PromiseAsyncGeneratorJob into the job queue.
 */
void
ecma_enqueue_promise_async_generator_job (ecma_value_t executable_object) /**< executable object */
{
  ecma_job_promise_async_generator_t *job_p;
  job_p = (ecma_job_promise_async_generator_t *) jmem_heap_alloc_block (sizeof (ecma_job_promise_async_generator_t));
  job_p->header.next_and_type = ECMA_JOB_PROMISE_ASYNC_GENERATOR;
  job_p->executable_object = ecma_copy_value (executable_object);

  ecma_enqueue_job (&job_p->header);
} /* ecma_enqueue_promise_async_generator_job */

/**
 * Enqueue a PromiseResolveThenableJob into the job queue.
 */
void
ecma_enqueue_promise_resolve_thenable_job (ecma_value_t promise, /**< promise to be resolved */
                                           ecma_value_t thenable, /**< thenable object */
                                           ecma_value_t then) /**< 'then' function */
{
  JERRY_ASSERT (ecma_is_promise (ecma_get_object_from_value (promise)));
  JERRY_ASSERT (ecma_is_value_object (thenable));
  JERRY_ASSERT (ecma_op_is_callable (then));

  ecma_job_promise_resolve_thenable_t *job_p;
  job_p = (ecma_job_promise_resolve_thenable_t *) jmem_heap_alloc_block (sizeof (ecma_job_promise_resolve_thenable_t));
  job_p->header.next_and_type = ECMA_JOB_PROMISE_THENABLE;
  job_p->promise = ecma_copy_value (promise);
  job_p->thenable = ecma_copy_value (thenable);
  job_p->then = ecma_copy_value (then);

  ecma_enqueue_job (&job_p->header);
} /* ecma_enqueue_promise_resolve_thenable_job */

/**
 * Process enqueued Promise jobs until the first thrown error or until the
 * jobqueue becomes empty.
 *
 * @return result of the last processed job - if the jobqueue was non-empty,
 *         undefined - otherwise.
 */
ecma_value_t
ecma_process_all_enqueued_jobs (void)
{
  ecma_value_t ret = ECMA_VALUE_UNDEFINED;

  while (JERRY_CONTEXT (job_queue_head_p) != NULL && !ECMA_IS_VALUE_ERROR (ret))
  {
    ecma_job_queue_item_t *job_p = JERRY_CONTEXT (job_queue_head_p);
    JERRY_CONTEXT (job_queue_head_p) = ecma_job_queue_get_next (job_p);

    ecma_fast_free_value (ret);

    switch (ecma_job_queue_get_type (job_p))
    {
      case ECMA_JOB_PROMISE_REACTION:
      {
        ret = ecma_process_promise_reaction_job ((ecma_job_promise_reaction_t *) job_p);
        break;
      }
      case ECMA_JOB_PROMISE_ASYNC_REACTION_FULFILLED:
      case ECMA_JOB_PROMISE_ASYNC_REACTION_REJECTED:
      {
        ret = ecma_process_promise_async_reaction_job ((ecma_job_promise_async_reaction_t *) job_p);
        break;
      }
      case ECMA_JOB_PROMISE_ASYNC_GENERATOR:
      {
        ecma_process_promise_async_generator_job ((ecma_job_promise_async_generator_t *) job_p);
        break;
      }
      default:
      {
        JERRY_ASSERT (ecma_job_queue_get_type (job_p) == ECMA_JOB_PROMISE_THENABLE);

        ret = ecma_process_promise_resolve_thenable_job ((ecma_job_promise_resolve_thenable_t *) job_p);
        break;
      }
    }
  }

  return ret;
} /* ecma_process_all_enqueued_jobs */

/**
 * Release enqueued Promise jobs.
 */
void
ecma_free_all_enqueued_jobs (void)
{
  while (JERRY_CONTEXT (job_queue_head_p) != NULL)
  {
    ecma_job_queue_item_t *job_p = JERRY_CONTEXT (job_queue_head_p);
    JERRY_CONTEXT (job_queue_head_p) = ecma_job_queue_get_next (job_p);

    switch (ecma_job_queue_get_type (job_p))
    {
      case ECMA_JOB_PROMISE_REACTION:
      {
        ecma_free_promise_reaction_job ((ecma_job_promise_reaction_t *) job_p);
        break;
      }
      case ECMA_JOB_PROMISE_ASYNC_REACTION_FULFILLED:
      case ECMA_JOB_PROMISE_ASYNC_REACTION_REJECTED:
      {
        ecma_free_promise_async_reaction_job ((ecma_job_promise_async_reaction_t *) job_p);
        break;
      }
      case ECMA_JOB_PROMISE_ASYNC_GENERATOR:
      {
        ecma_free_promise_async_generator_job ((ecma_job_promise_async_generator_t *) job_p);
        break;
      }
      default:
      {
        JERRY_ASSERT (ecma_job_queue_get_type (job_p) == ECMA_JOB_PROMISE_THENABLE);

        ecma_free_promise_resolve_thenable_job ((ecma_job_promise_resolve_thenable_t *) job_p);
        break;
      }
    }
  }
} /* ecma_free_all_enqueued_jobs */

/**
 * @}
 * @}
 */
#endif /* ENABLED (JERRY_BUILTIN_PROMISE) */
