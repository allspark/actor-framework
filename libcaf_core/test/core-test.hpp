#pragma once

#include "caf/test/bdd_dsl.hpp"

#include "caf/binary_deserializer.hpp"
#include "caf/binary_serializer.hpp"
#include "caf/byte_buffer.hpp"
#include "caf/cow_vector.hpp"
#include "caf/fwd.hpp"
#include "caf/raise_error.hpp"
#include "caf/result.hpp"
#include "caf/type_id.hpp"
#include "caf/typed_actor.hpp"
#include "caf/typed_stream.hpp"

#include <cstdint>
#include <numeric>
#include <string>
#include <utility>

// -- utility for testing flows ------------------------------------------------

namespace caf::flow {

/// Represents the current state of an @ref observer.
enum class observer_state {
  /// Indicates that no callbacks were called yet.
  idle,
  /// Indicates that on_subscribe was called.
  subscribed,
  /// Indicates that on_complete was called.
  completed,
  /// Indicates that on_error was called.
  aborted
};

/// Returns whether `x` represents a final state, i.e., `completed`, `aborted`
/// or `disposed`.
constexpr bool is_final(observer_state x) noexcept {
  return static_cast<int>(x) >= static_cast<int>(observer_state::completed);
}

/// Returns whether `x` represents an active state, i.e., `idle` or
/// `subscribed`.
constexpr bool is_active(observer_state x) noexcept {
  return static_cast<int>(x) <= static_cast<int>(observer_state::subscribed);
}

/// @relates observer_state
std::string to_string(observer_state);

/// @relates observer_state
bool from_string(std::string_view, observer_state&);

/// @relates observer_state
bool from_integer(std::underlying_type_t<observer_state>, observer_state&);

/// @relates observer_state
template <class Inspector>
bool inspect(Inspector& f, observer_state& x) {
  return default_enum_inspect(f, x);
}

/// Returns a trivial disposable that wraps an atomic flag.
disposable make_trivial_disposable();

/// An observer with minimal internal logic. Useful for writing unit tests.
template <class T>
class passive_observer : public observer_impl_base<T> {
public:
  // -- implementation of observer_impl<T> -------------------------------------

  void on_complete() override {
    if (sub) {
      subscription tmp;
      tmp.swap(sub);
      tmp.dispose();
    }
    state = observer_state::completed;
  }

  void on_error(const error& what) override {
    if (sub) {
      sub.dispose();
      sub = nullptr;
    }
    err = what;
    state = observer_state::aborted;
  }

  void on_subscribe(subscription new_sub) override {
    if (state == observer_state::idle) {
      CAF_ASSERT(!sub);
      sub = std::move(new_sub);
      state = observer_state::subscribed;
    } else {
      new_sub.dispose();
    }
  }

  void on_next(const T& item) override {
    if (!subscribed()) {
      auto what = "on_next called but observer is in state " + to_string(state);
      CAF_RAISE_ERROR(std::logic_error, what.c_str());
    }
    buf.emplace_back(item);
  }

  // -- convenience functions --------------------------------------------------

  bool request(size_t demand) {
    if (sub) {
      sub.request(demand);
      return true;
    } else {
      return false;
    }
  }

  void unsubscribe() {
    if (sub) {
      sub.dispose();
      state = observer_state::idle;
    }
  }

  bool idle() const noexcept {
    return state == observer_state::idle;
  }

  bool subscribed() const noexcept {
    return state == observer_state::subscribed;
  }

  bool completed() const noexcept {
    return state == observer_state::completed;
  }

  bool aborted() const noexcept {
    return state == observer_state::aborted;
  }

  std::vector<T> sorted_buf() const {
    auto result = buf;
    std::sort(result.begin(), result.end());
    return result;
  }

  // -- member variables -------------------------------------------------------

  /// The subscription for requesting additional items.
  subscription sub;

  /// Default-constructed unless on_error was called.
  error err;

  /// Represents the current state of this observer.
  observer_state state = observer_state::idle;

  /// Stores all items received via `on_next`.
  std::vector<T> buf;
};

template <class T>
struct unsubscribe_guard {
public:
  explicit unsubscribe_guard(intrusive_ptr<T> ptr) : ptr_(std::move(ptr)) {
    // nop
  }

  unsubscribe_guard(unsubscribe_guard&&) = default;

  unsubscribe_guard& operator=(unsubscribe_guard&&) = delete;

  unsubscribe_guard(const unsubscribe_guard&) = delete;

  unsubscribe_guard& operator=(const unsubscribe_guard&) = delete;

  ~unsubscribe_guard() {
    if (ptr_)
      ptr_->unsubscribe();
  }

private:
  intrusive_ptr<T> ptr_;
};

template <class T>
auto make_unsubscribe_guard(intrusive_ptr<T> ptr) {
  return unsubscribe_guard<T>{std::move(ptr)};
}

template <class T1, class T2, class... Ts>
auto make_unsubscribe_guard(intrusive_ptr<T1> ptr1, intrusive_ptr<T2> ptr2,
                            Ts... ptrs) {
  return std::make_tuple(make_unsubscribe_guard(std::move(ptr1)),
                         make_unsubscribe_guard(ptr2),
                         make_unsubscribe_guard(std::move(ptrs))...);
}

template <class T>
class canceling_observer : public flow::observer_impl_base<T> {
public:
  explicit canceling_observer(bool accept_first) : accept_next(accept_first) {
    // nop
  }

  void on_next(const T&) override {
    ++on_next_calls;
    if (sub) {
      sub.dispose();
      sub = nullptr;
    }
  }

  void on_error(const error&) override {
    ++on_error_calls;
    if (sub)
      sub = nullptr;
  }

  void on_complete() override {
    ++on_complete_calls;
    if (sub)
      sub = nullptr;
  }

  void on_subscribe(flow::subscription sub) override {
    if (accept_next) {
      accept_next = false;
      sub.request(128);
      this->sub = std::move(sub);
      return;
    }
    sub.dispose();
  }

  int on_next_calls = 0;
  int on_error_calls = 0;
  int on_complete_calls = 0;
  bool accept_next = false;
  flow::subscription sub;
};

template <class T>
auto make_canceling_observer(bool accept_first = false) {
  return make_counted<canceling_observer<T>>(accept_first);
}

/// @relates passive_observer
template <class T>
intrusive_ptr<passive_observer<T>> make_passive_observer() {
  return make_counted<passive_observer<T>>();
}

/// Similar to @ref passive_observer but automatically requests items until
/// completed. Useful for writing unit tests.
template <class T>
class auto_observer : public passive_observer<T> {
public:
  using super = passive_observer<T>;

  void on_subscribe(subscription new_sub) override {
    if (this->state == observer_state::idle) {
      CAF_ASSERT(!this->sub);
      this->sub = std::move(new_sub);
      this->state = observer_state::subscribed;
      this->sub.request(64);
    } else {
      new_sub.dispose();
    }
  }

  void on_next(const T& item) override {
    super::on_next(item);
    if (this->sub)
      this->sub.request(1);
  }
};

/// @relates auto_observer
template <class T>
intrusive_ptr<auto_observer<T>> make_auto_observer() {
  return make_counted<auto_observer<T>>();
}

/// A subscription implementation without internal logic.
class passive_subscription_impl final : public subscription::impl_base {
public:
  /// Incremented by `request`.
  size_t demand = 0;

  /// Flipped by `dispose`.
  bool disposed_flag = false;

  void request(size_t n) override;

  void dispose() override;

  bool disposed() const noexcept override;
};

inline auto make_passive_subscription() {
  return make_counted<passive_subscription_impl>();
}

namespace op {

/// An observable that does nothing when subscribed except returning a trivial
/// disposable. Allows tests to call on_subscribe some time later.
template <class T>
class nil_observable : public op::cold<T> {
public:
  using super = op::cold<T>;

  using shared_count = std::shared_ptr<size_t>;

  nil_observable(coordinator* ctx, shared_count subscribe_count)
    : super(ctx), subscribe_count_(std::move(subscribe_count)) {
    // nop
  }

  disposable subscribe(observer<T>) override {
    if (subscribe_count_)
      *subscribe_count_ += 1;
    return make_trivial_disposable();
  }

  shared_count subscribe_count_;
};

/// An observable that passes a trivial disposable to any observer.
template <class T>
class trivial_observable : public op::cold<T> {
public:
  using super = op::cold<T>;

  using shared_count = std::shared_ptr<size_t>;

  trivial_observable(coordinator* ctx, shared_count subscribe_count)
    : super(ctx), subscribe_count_(std::move(subscribe_count)) {
    // nop
  }

  disposable subscribe(observer<T> out) override {
    if (subscribe_count_)
      *subscribe_count_ += 1;
    auto ptr = make_counted<passive_subscription_impl>();
    out.on_subscribe(subscription{ptr});
    return make_trivial_disposable();
  }

  shared_count subscribe_count_;
};

} // namespace op

template <class T>
observable<T>
make_nil_observable(coordinator* ctx,
                    std::shared_ptr<size_t> subscribe_count = nullptr) {
  auto ptr = make_counted<op::nil_observable<T>>(ctx, subscribe_count);
  return observable<T>{std::move(ptr)};
}

template <class T>
observable<T>
make_trivial_observable(coordinator* ctx,
                        std::shared_ptr<size_t> subscribe_count = nullptr) {
  auto ptr = make_counted<op::trivial_observable<T>>(ctx, subscribe_count);
  return observable<T>{std::move(ptr)};
}

} // namespace caf::flow

// -- utility for testing serialization round-trips ----------------------------

template <class T>
T deep_copy(const T& val) {
  using namespace std::literals;
  caf::byte_buffer buf;
  {
    caf::binary_serializer sink{nullptr, buf};
    if (!sink.apply(val)) {
      auto msg = "serialization failed in deep_copy: "s;
      msg += to_string(sink.get_error());
      CAF_RAISE_ERROR(msg.c_str());
    }
  }
  auto result = T{};
  {
    caf::binary_deserializer sink{nullptr, buf};
    if (!sink.apply(result)) {
      auto msg = "deserialization failed in deep_copy: "s;
      msg += to_string(sink.get_error());
      CAF_RAISE_ERROR(msg.c_str());
    }
  }
  return result;
}

// -- forward declarations for all unit test suites ----------------------------

using float_actor = caf::typed_actor<caf::result<void>(float)>;

using int_actor = caf::typed_actor<caf::result<int32_t>(int32_t)>;

using foo_actor
  = caf::typed_actor<caf::result<int32_t>(int32_t, int32_t, int32_t),
                     caf::result<double, double>(double)>;

// A simple POD type.
struct dummy_struct {
  int a;
  std::string b;
};

[[maybe_unused]] inline bool operator==(const dummy_struct& x,
                                        const dummy_struct& y) {
  return x.a == y.a && x.b == y.b;
}

template <class Inspector>
bool inspect(Inspector& f, dummy_struct& x) {
  return f.object(x).fields(f.field("a", x.a), f.field("b", x.b));
}

// An empty type.
struct dummy_tag_type {};

constexpr bool operator==(dummy_tag_type, dummy_tag_type) {
  return true;
}

// Fails the test when copied. Implemented in message_lifetime.cpp.
struct fail_on_copy {
  int value;

  fail_on_copy() : value(0) {
    // nop
  }

  explicit fail_on_copy(int x) : value(x) {
    // nop
  }

  fail_on_copy(fail_on_copy&&) = default;

  fail_on_copy& operator=(fail_on_copy&&) = default;

  fail_on_copy(const fail_on_copy&);

  fail_on_copy& operator=(const fail_on_copy&);

  template <class Inspector>
  friend bool inspect(Inspector& f, fail_on_copy& x) {
    return f.object(x).fields(f.field("value", x.value));
  }
};

struct i32_wrapper {
  // Initialized in meta_object.cpp.
  static size_t instances;

  int32_t value;

  i32_wrapper() : value(0) {
    ++instances;
  }

  ~i32_wrapper() {
    --instances;
  }

  template <class Inspector>
  friend bool inspect(Inspector& f, i32_wrapper& x) {
    return f.apply(x.value);
  }
};

struct i64_wrapper {
  // Initialized in meta_object.cpp.
  static size_t instances;

  int64_t value;

  i64_wrapper() : value(0) {
    ++instances;
  }

  explicit i64_wrapper(int64_t val) : value(val) {
    ++instances;
  }

  ~i64_wrapper() {
    --instances;
  }

  template <class Inspector>
  friend bool inspect(Inspector& f, i64_wrapper& x) {
    return f.apply(x.value);
  }
};

struct my_request {
  int32_t a = 0;
  int32_t b = 0;
  my_request() = default;
  my_request(int a, int b) : a(a), b(b) {
    // nop
  }
};

[[maybe_unused]] inline bool operator==(const my_request& x,
                                        const my_request& y) {
  return std::tie(x.a, x.b) == std::tie(y.a, y.b);
}

template <class Inspector>
bool inspect(Inspector& f, my_request& x) {
  return f.object(x).fields(f.field("a", x.a), f.field("b", x.b));
}

struct raw_struct {
  std::string str;
};

template <class Inspector>
bool inspect(Inspector& f, raw_struct& x) {
  return f.object(x).fields(f.field("str", x.str));
}

[[maybe_unused]] inline bool operator==(const raw_struct& lhs,
                                        const raw_struct& rhs) {
  return lhs.str == rhs.str;
}

struct s1 {
  int value[3] = {10, 20, 30};
};

template <class Inspector>
bool inspect(Inspector& f, s1& x) {
  return f.apply(x.value);
}

struct s2 {
  int value[4][2] = {{1, 10}, {2, 20}, {3, 30}, {4, 40}};
};

template <class Inspector>
bool inspect(Inspector& f, s2& x) {
  return f.apply(x.value);
}

struct s3 {
  std::array<int, 4> value;
  s3() {
    std::iota(value.begin(), value.end(), 1);
  }
};

template <class Inspector>
bool inspect(Inspector& f, s3& x) {
  return f.apply(x.value);
}

struct test_array {
  int32_t value[4];
  int32_t value2[2][4];
};

template <class Inspector>
bool inspect(Inspector& f, test_array& x) {
  return f.object(x).fields(f.field("value", x.value),
                            f.field("value2", x.value2));
}

// Implemented in serialization.cpp.
struct test_empty_non_pod {
  test_empty_non_pod() = default;
  test_empty_non_pod(const test_empty_non_pod&) = default;
  test_empty_non_pod& operator=(const test_empty_non_pod&) = default;
  virtual void foo();
  virtual ~test_empty_non_pod();
};

template <class Inspector>
bool inspect(Inspector& f, test_empty_non_pod& x) {
  return f.object(x).fields();
}

enum class test_enum : int32_t {
  a,
  b,
  c,
};

// Implemented in serialization.cpp
std::string to_string(test_enum x);

template <class Inspector>
bool inspect(Inspector& f, test_enum& x) {
  auto get = [&x] { return static_cast<int32_t>(x); };
  auto set = [&x](int32_t val) {
    if (val >= 0 && val <= 2) {
      x = static_cast<test_enum>(val);
      return true;
    } else {
      return false;
    }
  };
  return f.apply(get, set);
}

// Used in serializer.cpp and deserializer.cpp
struct test_data {
  int32_t i32;
  int64_t i64;
  float f32;
  double f64;
  caf::timestamp ts;
  test_enum te;
  std::string str;
};

template <class Inspector>
bool inspect(Inspector& f, test_data& x) {
  return f.object(x).fields(f.field("i32", x.i32), f.field("i64", x.i64),
                            f.field("f32", x.f32), f.field("f64", x.f64),
                            f.field("ts", x.ts), f.field("te", x.te),
                            f.field("str", x.str));
}

[[maybe_unused]] inline bool operator==(const test_data& x,
                                        const test_data& y) {
  return std::tie(x.i32, x.i64, x.f32, x.f64, x.ts, x.te, x.str)
         == std::tie(y.i32, y.i64, y.f32, y.f64, y.ts, y.te, y.str);
}

enum class dummy_enum_class : short { foo, bar };

[[maybe_unused]] inline std::string to_string(dummy_enum_class x) {
  return x == dummy_enum_class::foo ? "foo" : "bar";
}

template <class Inspector>
bool inspect(Inspector& f, dummy_enum_class& x) {
  auto get = [&x] { return static_cast<short>(x); };
  auto set = [&x](short val) {
    if (val >= 0 && val <= 1) {
      x = static_cast<dummy_enum_class>(val);
      return true;
    } else {
      return false;
    }
  };
  return f.apply(get, set);
}

enum class level : uint8_t { all, trace, debug, warning, error };

std::string to_string(level);

bool from_string(std::string_view, level&);

bool from_integer(uint8_t, level&);

template <class Inspector>
bool inspect(Inspector& f, level& x) {
  return caf::default_enum_inspect(f, x);
}

enum dummy_enum { de_foo, de_bar };

template <class Inspector>
bool inspect(Inspector& f, dummy_enum& x) {
  using integer_type = std::underlying_type_t<dummy_enum>;
  auto get = [&x] { return static_cast<integer_type>(x); };
  auto set = [&x](integer_type val) {
    if (val <= 1) {
      x = static_cast<dummy_enum>(val);
      return true;
    } else {
      return false;
    }
  };
  return f.apply(get, set);
}

struct point {
  int32_t x;
  int32_t y;
};

[[maybe_unused]] constexpr bool operator==(point a, point b) noexcept {
  return a.x == b.x && a.y == b.y;
}

[[maybe_unused]] constexpr bool operator!=(point a, point b) noexcept {
  return !(a == b);
}

template <class Inspector>
bool inspect(Inspector& f, point& x) {
  return f.object(x).fields(f.field("x", x.x), f.field("y", x.y));
}

struct rectangle {
  point top_left;
  point bottom_right;
};

template <class Inspector>
bool inspect(Inspector& f, rectangle& x) {
  return f.object(x).fields(f.field("top-left", x.top_left),
                            f.field("bottom-right", x.bottom_right));
}

[[maybe_unused]] constexpr bool operator==(const rectangle& x,
                                           const rectangle& y) noexcept {
  return x.top_left == y.top_left && x.bottom_right == y.bottom_right;
}

[[maybe_unused]] constexpr bool operator!=(const rectangle& x,
                                           const rectangle& y) noexcept {
  return !(x == y);
}

struct circle {
  point center;
  int32_t radius;
};

template <class Inspector>
bool inspect(Inspector& f, circle& x) {
  return f.object(x).fields(f.field("center", x.center),
                            f.field("radius", x.radius));
}

[[maybe_unused]] constexpr bool operator==(const circle& x,
                                           const circle& y) noexcept {
  return x.center == y.center && x.radius == y.radius;
}

[[maybe_unused]] constexpr bool operator!=(const circle& x,
                                           const circle& y) noexcept {
  return !(x == y);
}

struct widget {
  std::string color;
  std::variant<rectangle, circle> shape;
};

template <class Inspector>
bool inspect(Inspector& f, widget& x) {
  return f.object(x).fields(f.field("color", x.color),
                            f.field("shape", x.shape));
}

[[maybe_unused]] inline bool operator==(const widget& x,
                                        const widget& y) noexcept {
  return x.color == y.color && x.shape == y.shape;
}

[[maybe_unused]] inline bool operator!=(const widget& x,
                                        const widget& y) noexcept {
  return !(x == y);
}

struct dummy_user {
  std::string name;
  std::optional<std::string> nickname;
};

template <class Inspector>
bool inspect(Inspector& f, dummy_user& x) {
  return f.object(x).fields(f.field("name", x.name),
                            f.field("nickname", x.nickname));
}

struct phone_book {
  std::string city;
  std::map<std::string, int64_t> entries;
};

[[maybe_unused]] constexpr bool operator==(const phone_book& x,
                                           const phone_book& y) noexcept {
  return std::tie(x.city, x.entries) == std::tie(y.city, y.entries);
}

[[maybe_unused]] constexpr bool operator!=(const phone_book& x,
                                           const phone_book& y) noexcept {
  return !(x == y);
}

template <class Inspector>
bool inspect(Inspector& f, phone_book& x) {
  return f.object(x).fields(f.field("city", x.city),
                            f.field("entries", x.entries));
}

// -- type IDs for for all unit test suites ------------------------------------

#define ADD_TYPE_ID(type) CAF_ADD_TYPE_ID(core_test, type)
#define ADD_ATOM(atom_name) CAF_ADD_ATOM(core_test, atom_name)

CAF_BEGIN_TYPE_ID_BLOCK(core_test, caf::first_custom_type_id)

  ADD_TYPE_ID((caf::cow_vector<int>) )
  ADD_TYPE_ID((caf::typed_stream<int32_t>) )
  ADD_TYPE_ID((circle))
  ADD_TYPE_ID((dummy_enum))
  ADD_TYPE_ID((dummy_enum_class))
  ADD_TYPE_ID((dummy_struct))
  ADD_TYPE_ID((dummy_tag_type))
  ADD_TYPE_ID((dummy_user))
  ADD_TYPE_ID((fail_on_copy))
  ADD_TYPE_ID((float_actor))
  ADD_TYPE_ID((foo_actor))
  ADD_TYPE_ID((i32_wrapper))
  ADD_TYPE_ID((i64_wrapper))
  ADD_TYPE_ID((int_actor))
  ADD_TYPE_ID((level))
  ADD_TYPE_ID((my_request))
  ADD_TYPE_ID((phone_book))
  ADD_TYPE_ID((point))
  ADD_TYPE_ID((raw_struct))
  ADD_TYPE_ID((rectangle))
  ADD_TYPE_ID((s1))
  ADD_TYPE_ID((s2))
  ADD_TYPE_ID((s3))
  ADD_TYPE_ID((std::map<int32_t, int32_t>) )
  ADD_TYPE_ID((std::map<std::string, std::u16string>) )
  ADD_TYPE_ID((std::pair<level, std::string>) )
  ADD_TYPE_ID((std::tuple<int32_t, int32_t, int32_t>) )
  ADD_TYPE_ID((std::tuple<std::string, int32_t, uint32_t>) )
  ADD_TYPE_ID((std::vector<bool>) )
  ADD_TYPE_ID((std::vector<int32_t>) )
  ADD_TYPE_ID((std::vector<std::pair<level, std::string>>) )
  ADD_TYPE_ID((std::vector<std::string>) )
  ADD_TYPE_ID((test_array))
  ADD_TYPE_ID((test_empty_non_pod))
  ADD_TYPE_ID((test_enum))
  ADD_TYPE_ID((widget))

  ADD_ATOM(abc_atom)
  ADD_ATOM(get_state_atom)
  ADD_ATOM(name_atom)
  ADD_ATOM(sub0_atom)
  ADD_ATOM(sub1_atom)
  ADD_ATOM(sub2_atom)
  ADD_ATOM(sub3_atom)
  ADD_ATOM(sub4_atom)
  ADD_ATOM(hi_atom)
  ADD_ATOM(ho_atom)

CAF_END_TYPE_ID_BLOCK(core_test)

#undef ADD_TYPE_ID
#undef ADD_ATOM
