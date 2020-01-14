/*
 * xwalk_cookie_change_dispatcher_wrapper.cc
 *
 *  Created on: Jan 14, 2020
 *      Author: iotto
 */

#include "xwalk/runtime/browser/android/net/xwalk_cookie_change_dispatcher_wrapper.h"

#include "base/bind.h"
#include "base/memory/ref_counted_delete_on_sequence.h"
#include "base/threading/thread_task_runner_handle.h"
#include "net/cookies/canonical_cookie.h"
#include "net/cookies/cookie_store.h"
#include "url/gurl.h"
#include "xwalk/runtime/browser/android/cookie_manager.h"

namespace xwalk {

namespace {
// Posts |task| to the thread that the global CookieStore lives on.
void PostTaskToCookieStoreTaskRunner(base::OnceClosure task) {
  GetCookieStoreTaskRunner()->PostTask(FROM_HERE, std::move(task));
}

using CookieChangeCallbackList =
base::CallbackList<void(const net::CanonicalCookie& cookie,
    net::CookieChangeCause cause)>;

class XWalkCookieChangeSubscription : public net::CookieChangeSubscription {
 public:
  explicit XWalkCookieChangeSubscription(std::unique_ptr<CookieChangeCallbackList::Subscription> subscription)
      : subscription_(std::move(subscription)) {
  }

 private:
  std::unique_ptr<CookieChangeCallbackList::Subscription> subscription_;

  DISALLOW_COPY_AND_ASSIGN(XWalkCookieChangeSubscription);
};

// Wraps a subscription to cookie change notifications for the global
// CookieStore for a consumer that lives on another thread. Handles passing
// messages between threads, and destroys itself when the consumer unsubscribes.
// Must be created on the consumer's thread. Each instance only supports a
// single subscription.
class SubscriptionWrapper {
 public:
  SubscriptionWrapper()
      : weak_factory_(this) {
  }

  enum Mode {
    kByCookie,
    kByUrl,
  };

  std::unique_ptr<net::CookieChangeSubscription> Subscribe(Mode mode, const GURL& url, const std::string& name,
                                                           net::CookieChangeCallback callback) {
    // This class is only intended to be used for a single subscription.
    DCHECK(callback_list_.empty());

    nested_subscription_ = NestedSubscription::Create(mode, url, name, weak_factory_.GetWeakPtr());
    return std::make_unique<XWalkCookieChangeSubscription>(callback_list_.Add(std::move(callback)));
  }

 private:
  // The NestedSubscription is responsible for creating and managing the
  // underlying subscription to the real CookieStore, and posting notifications
  // back to |callback_list_|.
  class NestedSubscription : public base::RefCountedDeleteOnSequence<NestedSubscription> {
   public:
    static scoped_refptr<NestedSubscription> Create(Mode mode, const GURL& url, const std::string& name,
                                                    base::WeakPtr<SubscriptionWrapper> subscription_wrapper) {
      auto subscription = base::WrapRefCounted(new NestedSubscription(std::move(subscription_wrapper)));
      PostTaskToCookieStoreTaskRunner(base::BindOnce(&NestedSubscription::Subscribe, subscription, mode, url, name));
      return subscription;
    }

   private:
    friend class base::RefCountedDeleteOnSequence<NestedSubscription>;
    friend class base::DeleteHelper<NestedSubscription>;

    explicit NestedSubscription(base::WeakPtr<SubscriptionWrapper> subscription_wrapper)
        : base::RefCountedDeleteOnSequence<NestedSubscription>(GetCookieStoreTaskRunner()),
          subscription_wrapper_(subscription_wrapper),
          client_task_runner_(base::ThreadTaskRunnerHandle::Get()) {
    }

    ~NestedSubscription() {
    }

    void Subscribe(Mode mode, const GURL& url, const std::string& name) {
      switch (mode) {
        case kByCookie:
          subscription_ = GetCookieStore()->GetChangeDispatcher().AddCallbackForCookie(
              url, name, base::BindRepeating(&NestedSubscription::OnChanged, this));
          break;
        case kByUrl:
          subscription_ = GetCookieStore()->GetChangeDispatcher().AddCallbackForUrl(
              url, base::BindRepeating(&NestedSubscription::OnChanged, this));
      }
    }

    void OnChanged(const net::CanonicalCookie& cookie, net::CookieChangeCause cause) {
      client_task_runner_->PostTask(
      FROM_HERE,base::BindOnce(&SubscriptionWrapper::OnChanged,
          subscription_wrapper_, cookie, cause));
    }

    base::WeakPtr<SubscriptionWrapper> subscription_wrapper_;
    scoped_refptr<base::TaskRunner> client_task_runner_;

    std::unique_ptr<net::CookieChangeSubscription> subscription_;

    DISALLOW_COPY_AND_ASSIGN(NestedSubscription);
  };

  void OnChanged(const net::CanonicalCookie& cookie,
  net::CookieChangeCause cause) {
    callback_list_.Notify(cookie, cause);
  }

  void OnUnsubscribe() {delete this;}

  scoped_refptr<NestedSubscription> nested_subscription_;
  CookieChangeCallbackList callback_list_;
  base::WeakPtrFactory<SubscriptionWrapper> weak_factory_;

  DISALLOW_COPY_AND_ASSIGN(SubscriptionWrapper);
};

} // anonymous namespace

XWalkCookieChangeDispatcherWrapper::XWalkCookieChangeDispatcherWrapper() {
}

XWalkCookieChangeDispatcherWrapper::~XWalkCookieChangeDispatcherWrapper() = default;

std::unique_ptr<net::CookieChangeSubscription> XWalkCookieChangeDispatcherWrapper::AddCallbackForCookie(
    const GURL& url, const std::string& name, net::CookieChangeCallback callback) {

  // The SubscriptionWrapper is owned by the subscription itself, and has no
  // connection to the AwCookieStoreWrapper after creation. Other CookieStore
  // implementations DCHECK if a subscription outlasts the cookie store,
  // unfortunately, this design makes DCHECKing if there's an outstanding
  // subscription when the AwCookieStoreWrapper is destroyed a bit ugly.
  // TODO(mmenke):  Still worth adding a DCHECK?
  SubscriptionWrapper* subscription = new SubscriptionWrapper();
  return subscription->Subscribe(SubscriptionWrapper::kByCookie, url, name, std::move(callback));
}

std::unique_ptr<net::CookieChangeSubscription> XWalkCookieChangeDispatcherWrapper::AddCallbackForUrl(
    const GURL& url, net::CookieChangeCallback callback) {

  SubscriptionWrapper* subscription = new SubscriptionWrapper();
  return subscription->Subscribe(SubscriptionWrapper::kByUrl, url,
  /* name=, ignored */"",
                                 std::move(callback));
}

std::unique_ptr<net::CookieChangeSubscription> XWalkCookieChangeDispatcherWrapper::AddCallbackForAllChanges(
    net::CookieChangeCallback callback) {
  // Implement when needed by Android Webview consumers.
  NOTIMPLEMENTED();
  return nullptr;
}

}  // namespace xwalk
