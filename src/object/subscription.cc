/*
 * Copyright (C) 2011, Gostai S.A.S.
 *
 * This software is provided "as is" without warranty of any kind,
 * either expressed or implied, including but not limited to the
 * implied warranties of fitness for a particular purpose.
 *
 * See the LICENSE file for more information.
 */

#include <algorithm>

#include <kernel/userver.hh>
#include <object/symbols.hh>
#include <runner/job.hh>
#include <urbi/object/event.hh>
#include <urbi/object/event-handler.hh>
#include <urbi/object/lobby.hh>
#include <urbi/sdk.hh>

#include <eval/call.hh>
#include <eval/exec.hh>
#include <eval/send-message.hh>

#include <urbi/runner/raise.hh>

GD_CATEGORY(Urbi.Event);
namespace urbi
{
  namespace object
  {
    URBI_CXX_OBJECT_INIT(Subscription)
    {
      init_();
      BIND(asynchronous, asynchronous_);
      BIND(callCount, callCount_);
      BIND(disconnected, disconnected_);
      BIND(enabled, enabled_);
      BIND(lastCall, lastCall_);
      BIND(maxCallTime, maxCallTime_);
      BIND(maxParallelEvents, maxParallelEvents_);
      BIND(minCallTime, minCallTime_);
      BIND(minInterval, minInterval_);
      BIND(onEvent, onEvent_);
      BIND(processing, processing_);
      BIND(stop);
      BIND(totalCallTime, totalCallTime_);
    }

    Subscription::Subscription()
    {
      init_();
    }

    Subscription::Subscription(callback_type* cb)
    {
      init_();
      cb_ = cb;
    }

    Subscription::Subscription(rEvent source, rExecutable g,
                               rExecutable e,
                               rExecutable l, bool s)
    {
      init_();
      event_ = source;
      asynchronous_ = !s;
      guard = g;
      enter_ = e;
      leave_ = l;
    }

    Subscription::Subscription(libport::intrusive_ptr<Subscription> model)
    {
      init_();
      cb_ = model->cb_;
      enabled_ = model->enabled_get();
      minInterval_ = model->minInterval_get();
      onEvent_ = model->onEvent_get();
    }

    void
    Subscription::init_()
    {
      // FIXME: should we set lobby and tag_stack in all cases?
      // Only done for ats now.
      if (this != proto)
        proto_set(proto);
      cb_ = 0;
      frozen = 0;
      asynchronous_ = true;
      maxParallelEvents_ = 0;
      enabled_ = true;
      disconnected_ = false;
      processing_ = 0;
      minInterval_ = 0;
      if (!proto || this == proto)
        lastCall_ = 0;
      else
        lastCall_ = (ufloat)kernel::server().getTime() / 1.0e6;
      totalCallTime_ = 0;
      callCount_ = 0;
      maxCallTime_ = 0;
      minCallTime_ = 0;
    }

    Subscription::~Subscription()
    {
      foreach (boost::signals::connection& c, connections)
        c.disconnect();
    }

    void
    Subscription::stop()
    {
      disconnected_set(true);
    }

    void
    Subscription::unregister()
    {
      if (!event_)
        FRAISE("Unregister must be called on at-subscriptions only");
      // Asynchronous unregister
      stop();
      GD_FINFO_TRACE("Unregister registration %s on %s.", this, event_);

      foreach(boost::signals::connection& c, connections)
        c.disconnect();
      connections.clear();
      rEvent e = event_;
      // Since subscription removal is asynchronous, we have to lose refs of
      // those now.
      event_ = 0;
      lobby = 0;
      // We must handle unsubscribed_ now or watch condition will be evaluated
      foreach(rSubscription& s, e->callbacks_)
        if (!s->disconnected_get())
          return;
      if (!e->callbacks_.empty())
      {
        // No more subscribers now, but the event is not yet aware of
        // that.  Clear and call unsubscribed_ now instead of waiting
        // for next emit.
        GD_INFO_TRACE("No more subscribers, calling unsubscribed");
        e->callbacks_.clear();
        e->unsubscribed_();
      }
    }

    void
    Subscription::freeze()
    {
      GD_FINFO_TRACE("Freeze registration %s.", this);
      frozen++;
    }

    void
    Subscription::unfreeze()
    {
      GD_FINFO_TRACE("Unfreeze registration %s.", this);
      aver(frozen);
      frozen--;
    }

    void
    Subscription::run_(rExecutable e, objects_type& args)
    {
      aver(e);
      runner::Job& r = kernel::runner();
      call_stack_type cs = r.state.call_stack_get();
      FINALLY(((call_stack_type, cs))((runner::Job&, r)),
              r.state.call_stack_get() = cs);
      r.state
        .call_stack_get()
        .insert(r.state.call_stack_get().begin(),
                call_stack.begin(), call_stack.end());
      (*e)(args);
    }

    void
    Subscription::enter(objects_type& args)
    {
      if (enter_)
        run_(enter_, args);
    }

    void
    Subscription::leave(objects_type& args)
    {
      if (leave_)
        run_(leave_, args);
    }

    void
    Subscription::run_sync(rEvent src,
                           const objects_type& pl, EventHandler* h,
                           bool detach, bool sync)
    {
      GD_FPUSH_TRACE("Subscriber %s running synchronously in %s", this,
                     kernel::runner());
      bool threw = false;
      libport::utime_t now = kernel::server().getTime();
      try
      {
        processing_++;
        if (cb_)
          (*cb_)(pl);
        if (onEvent_)
        {
          runner::Job& j = kernel::runner();
          rObject jthis = j.state.this_get();
          FINALLY(((rObject, jthis))((runner::Job&, j)),
                  j.state.this_set(jthis));
          j.state.this_set(this);
          eval::call(kernel::runner(), onEvent_, pl);
        }
        if (event_)
        {
          if (h)
            // emit with duration case (trigger).
            h->trigger_job(this, detach);
          else
          {
            // Dirac emit case (not trigger)
            if (frozen)
            {
              GD_FINFO_TRACE("%s: Skip frozen registration %s.", src, this);
              return;
            }
            GD_FPUSH_TRACE("%s: Trigger registration %s.", src, this);
            objects_type args;
            args << src << src << new List(pl);
            rObject pattern = nil_class;
            if (guard)
              pattern = (*guard)(args);
            if (pattern != void_class)
            {
              args << pattern;
              if (detach && asynchronous_)
              {
                sched::rJob leaveJob;
                // We are already in a child job, so just spawn for one of the
                // two tasks.  Start jobs simultaneously.
                if (leave_)
                {
                  leaveJob = src->spawn_actions_job(lobby, call_stack,
                                                    leave_, profile, args);
                  leaveJob->start_job();
                }
                enter(args);
              }
              else
              {
                enter(args);
                leave(args);
              }
            }
          }
        } // (event)
      } // try
      catch (const UrbiException& e)
      {
        if (!sync
            ||
            !src->slot_get_value(SYMBOL(blockSubscriberException))->as_bool())
        {
          // Do not intercept the exception in async mode or if asked to
          --processing_;
          throw;
        }
        threw = true;
        eval::send_error("exception caught while processing Event:");
        eval::show_exception(e);
      }
      catch (const sched::exception&)
      {
        // Never intercept sched::exception.
        GD_FINFO_TRACE("Subscriber %s interrupted", this);
        --processing_;
        throw;
      }
      catch (...)
      {
        if (!sync
            ||
            !src->slot_get_value(SYMBOL(blockSubscriberException))->as_bool())
        {
          --processing_;
          throw;
        }
        threw = true;
        eval::send_error("unknown exception caught while processing Event");
      }

      if (threw)
      {
        bool b =
          src->slot_get_value(SYMBOL(unsubscribeFaultySubscriber))->as_bool();
        GD_FINFO_DUMP("Subscriber threw, removing if %s", b);
        if (b)
          stop();
      }
      GD_FINFO_TRACE("Subscriber %s terminating in %s", this,
                     kernel::runner());
      processing_--;
      libport::utime_t end = kernel::server().getTime();
      lastCall_ = (double)end / 1.0e6;
      double ct = (double)(end-now) / 1.0e6;
      minCallTime_ = callCount_ ? std::min(ct, minCallTime_) : ct;
      maxCallTime_ = std::max(ct, maxCallTime_);
      callCount_++;
      totalCallTime_ += ct;
    }
  }
}