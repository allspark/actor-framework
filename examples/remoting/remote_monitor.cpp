//
// Created by allspark on 2/4/25.
//

#include <caf/io/basp_broker.hpp>
#include <caf/io/middleman.hpp>

#include <caf/actor_registry.hpp>
#include <caf/actor_system_config.hpp>
#include <caf/event_based_actor.hpp>
#include <caf/exec_main.hpp>
#include <caf/scoped_actor.hpp>
#include <caf/typed_event_based_actor.hpp>

class Config : public caf::actor_system_config {
public:
  Config() {
    opt_group{custom_options_, "global"}
      .add(mode, "mode,m", "mode")
      .add(client, "client", "client");
  }

  std::uint16_t mode{0};
  bool client{false};
};

constexpr static std::uint16_t BASE_PORT = 2424;

using TestActor
  = caf::typed_actor<caf::result<caf::cow_string>(caf::cow_string)>;

TestActor::behavior_type createTestActor(TestActor::pointer self) {
  self->println("[TestActor] {}", self->address());
  return {
    [self, i = std::size_t{0}](const caf::cow_string& msg) mutable {
      self->println("[TestActor] sender: {}, msg: {}",
                    self->current_sender()->address(), msg);

      if (i == 2) {
        self->println("[TestActor] quiting actor: {}", i);
        self->quit(caf::exit_reason::user_shutdown);
      }

      return caf::cow_string{"msg counter " + std::to_string(++i)};
    },
  };
}

CAF_BEGIN_TYPE_ID_BLOCK(remoteMonitor, first_custom_type_id)
  CAF_ADD_TYPE_ID(remoteMonitor, (TestActor))
CAF_END_TYPE_ID_BLOCK(remoteMonitor)

using LookupActor
  = caf::typed_actor<caf::result<TestActor>(caf::registry_lookup_atom)>;

LookupActor::behavior_type createLookupActor(LookupActor::pointer self,
                                             std::uint16_t mode,
                                             caf::node_id node) {
  self->println("[Lookup-{}] {}", mode, self->address());
  auto basp
    = self->home_system().middleman().named_broker<caf::io::basp_broker>(
      "BASP");
  const auto id = caf::io::basp::header::config_server_id;

  return {
    [self, mode, node, basp,
     id](caf::registry_lookup_atom) -> caf::result<TestActor> {
      self->println("[Lookup-{}] get_atom, sender: {}", mode,
                    self->current_sender()->address());
      auto rp = self->make_response_promise<TestActor>();

      self
        ->mail(caf::forward_atom_v, node, id,
               caf::make_message(caf::registry_lookup_atom_v, "lookup"))
        .request(basp, caf::infinite)
        .then(
          [self, mode, rp](const caf::strong_actor_ptr& actor) mutable {
            if (actor) {
              self->println("[Lookup-{}] get next hop success: {}", mode,
                            actor->address());

              //              self->mail(caf::get_atom_v)
              //                .request(caf::actor_cast<LookupActor>(actor),
              //                caf::infinite) .then([rp](const TestActor& t)
              //                mutable { rp.deliver(t); });

              rp.delegate(caf::actor_cast<LookupActor>(actor),
                          caf::registry_lookup_atom_v);
              return;
            }
            self->println("[Lookup-{}] get next hop success: nullptr", mode);
            rp.deliver(caf::make_error(caf::sec::invalid_request));
          },
          [self, mode, rp](const caf::error& err) mutable {
            self->println("[Lookup-{}] error: {}", mode, err);
            rp.deliver(err);
          });

      return rp;
    },
  };
}

using ClientActor
  = caf::typed_actor<caf::result<void>(caf::contact_atom, caf::cow_string)>;

ClientActor::behavior_type createClient(ClientActor::pointer self,
                                        std::uint16_t mode,
                                        LookupActor lookupActor) {
  self->attach_functor([self, mode](const caf::error& error) {
    self->println("[Client-{}] exited: {}", mode, error);
  });

  return {[self, mode, lookupActor](caf::contact_atom,
                                    const caf::cow_string& msg) {
    self->mail(caf::registry_lookup_atom_v)
      .request(lookupActor, caf::infinite)
      .then(
        [self, mode, msg](const TestActor& testActor) {
          self->monitor(testActor, [self, mode,
                                    testActorAddr = testActor.address()](
                                     const caf::error& error) {
            self->println("[Client-{}] testActor {} offline, error: {}", mode,
                          testActorAddr, error);
          });
          self->println("[Client-{}] testActor {}", mode, testActor.address());
          self->mail(msg)
            .request(testActor, caf::infinite)
            .then([self, mode](const caf::cow_string& reply) {
              self->println("[Client-{}] reply: {}", mode, reply);
            });
        },
        [self, mode](const caf::error& error) {
          self->println("[Client-{}] lookup failed: {}", mode, error);
        });
  }};
}

int caf_main(caf::actor_system& system, const Config& cfg) {
  using namespace std::string_view_literals;

  const std::uint16_t listenPort = BASE_PORT + cfg.mode;
  if (auto r = system.middleman().open(listenPort, nullptr, true); !r) {
    system.println("failed open port {}, error: {}", listenPort, r.error());
    return 1;
  }

  if (cfg.mode == 0) {
    auto lookupTestActor = system.spawn(
      [mode = cfg.mode](
        LookupActor::pointer self) -> LookupActor::behavior_type {
        self->println("[Lookup-{}] {}", mode, self->address());
        auto testActor = self->spawn(createTestActor);
        return {
          [self, mode, testActor](caf::registry_lookup_atom) -> TestActor {
            self->println("[Lookup-{}] get_atom, sender: {}", mode,
                          self->current_sender()->address());
            return testActor;
          }};
      });
    system.registry().put("lookup", lookupTestActor);
    return 0;
  }

  auto node = system.middleman().connect("127.0.0.1",
                                         BASE_PORT + (cfg.mode - 1));
  if (!node) {
    system.println("failed to connect, error: {}", node.error());
    return 2;
  }

  auto lookup = system.spawn(createLookupActor, cfg.mode,
                             std::move(node).value());
  system.registry().put("lookup", lookup);

  if (cfg.client) {
    auto client = system.spawn(createClient, cfg.mode, lookup);
    caf::scoped_actor sc{system};
    auto reply = sc->mail(caf::contact_atom_v,
                          caf::cow_string{"client " + std::to_string(cfg.mode)})
                   .request(client, caf::infinite)
                   .receive();
    if (!reply) {
      system.println("sending to client error: {}", reply.error());
    }
  }

  return 0;
}

CAF_MAIN(caf::io::middleman, caf::id_block::remoteMonitor)