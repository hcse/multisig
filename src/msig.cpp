#include <eosio/action.hpp>
#include <eosio/crypto.hpp>
#include <eosio/permission.hpp>
#include <eosio/ignore.hpp>
#include <msig.hpp>

namespace hyphaspace
{

   void multisig::erasedoc (const checksum256 &hash)
   {
      _document_graph.erase_document(hash);
   }

   void multisig::propose(eosio::ignore<name> &proposer,
                          eosio::ignore<name> &proposal_name,
                          eosio::ignore<std::set<permission_level>> &requested,
                          eosio::ignore<std::vector<document_graph::content_group>> &content_groups,
                          eosio::ignore<transaction> &trx)
   {
      name _proposer;
      name _proposal_name;
      std::set<permission_level> _requested;
      transaction_header _trx_header;
      std::vector<document_graph::content_group> _content_groups;

      _ds >> _proposer >> _proposal_name >> _requested >> _content_groups;

      const char *trx_pos = _ds.pos();
      size_t size = _ds.remaining();
      _ds >> _trx_header;

      require_auth(_proposer);
      check(_trx_header.expiration >= eosio::time_point_sec(current_time_point()), "transaction expired");

      auto packed_requested = pack(_requested);

      auto res = check_transaction_authorization(
          trx_pos, size,
          (const char *)0, 0,
          packed_requested.data(), packed_requested.size());

      check(res > 0, "transaction authorization failed");

      document_graph::document doc = _document_graph.create_document(_proposer, _content_groups);

      std::vector<char> pkd_trans;
      pkd_trans.resize(size);
      memcpy((char *)pkd_trans.data(), trx_pos, size);

      proposals proptable(get_self(), get_self().value);
      check(proptable.find(_proposal_name.value) == proptable.end(), "proposal with the same name exists");

      proptable.emplace(get_self(), [&](auto &prop) {
         prop.proposer = _proposer;
         prop.proposal_name = _proposal_name;
         prop.packed_transaction = pkd_trans;
         prop.document_hash = doc.hash;
         prop.requested_approvals.reserve(_requested.size());
         for (auto &level : _requested)
         {
            prop.requested_approvals.push_back(approval{level, time_point{microseconds{0}}});
         }
      });
   }

   void multisig::approve(name proposer, name proposal_name, permission_level level,
                          const eosio::binary_extension<eosio::checksum256> &proposal_hash)
   {
      require_auth(level);

      if (proposal_hash)
      {
         proposals proptable(get_self(), proposer.value);
         auto &prop = proptable.get(proposal_name.value, "proposal not found");
         assert_sha256(prop.packed_transaction.data(), prop.packed_transaction.size(), *proposal_hash);
      }

      proposals proptable(get_self(), get_self().value);
      auto p_itr = proptable.find(proposal_name.value);
      check(p_itr != proptable.end(), "proposal does not exist: " + proposal_name.to_string());

      auto itr = std::find_if(p_itr->requested_approvals.begin(), p_itr->requested_approvals.end(), [&](const approval &a) { return a.level == level; });
      check(itr != p_itr->requested_approvals.end(), "approval is not on the list of requested approvals");

      proptable.modify(p_itr, get_self(), [&](auto &p) {
         p.provided_approvals.push_back(approval{level, current_time_point()});
         p.requested_approvals.erase(itr);
      });
   }

   void multisig::unapprove(name proposer, name proposal_name, permission_level level)
   {
      require_auth(level);

      proposals proptable(get_self(), get_self().value);
      auto p_itr = proptable.find(proposal_name.value);
      check(p_itr != proptable.end(), "proposal does not exist: " + proposal_name.to_string());
      
      auto itr = std::find_if(p_itr->provided_approvals.begin(), p_itr->provided_approvals.end(), [&](const approval &a) { return a.level == level; });
      check(itr != p_itr->provided_approvals.end(), "no approval previously granted");
      proptable.modify(p_itr, get_self(), [&](auto &p) {
         p.requested_approvals.push_back(approval{level, current_time_point()});
         p.provided_approvals.erase(itr);
      });
   }

   void multisig::cancel(name proposer, name proposal_name, name canceler)
   {
      require_auth(canceler);

      proposals proptable(get_self(), get_self().value);
      auto &prop = proptable.get(proposal_name.value, "proposal not found");

      if (canceler != proposer)
      {
         check(unpack<transaction_header>(prop.packed_transaction).expiration < eosio::time_point_sec(current_time_point()), "cannot cancel until expiration");
      }

      _document_graph.erase_document(prop.document_hash);
      proptable.erase(prop);
   }

   void multisig::exec(name proposer, name proposal_name, name executer)
   {
      require_auth(executer);

      proposals proptable(get_self(), get_self().value);
      auto &prop = proptable.get(proposal_name.value, "proposal not found");
      transaction_header trx_header;
      std::vector<action> context_free_actions;
      std::vector<action> actions;
      datastream<const char *> ds(prop.packed_transaction.data(), prop.packed_transaction.size());
      ds >> trx_header;
      check(trx_header.expiration >= eosio::time_point_sec(current_time_point()), "transaction expired");
      ds >> context_free_actions;
      check(context_free_actions.empty(), "not allowed to `exec` a transaction with context-free actions");
      ds >> actions;

      // approvals apptable(get_self(), proposer.value);
      // auto apps_it = apptable.find(proposal_name.value);
      std::vector<permission_level> approvals;
      invalidations inv_table(get_self(), get_self().value);
      // check(apps_it != apptable.end(), "approvals not found");

      approvals.reserve(prop.provided_approvals.size());
      for (auto &p : prop.provided_approvals)
      {
         auto it = inv_table.find(p.level.actor.value);
         if (it == inv_table.end() || it->last_invalidation_time < p.time)
         {
            approvals.push_back(p.level);
         }
      }

      auto packed_provided_approvals = pack(approvals);
      auto res = check_transaction_authorization(
          prop.packed_transaction.data(), prop.packed_transaction.size(),
          (const char *)0, 0,
          packed_provided_approvals.data(), packed_provided_approvals.size());

      check(res > 0, "transaction authorization failed");

      for (const auto &act : actions)
      {
         act.send();
      }

      _document_graph.erase_document(prop.document_hash);
      proptable.erase(prop);
   }

   void multisig::invalidate(name account)
   {
      require_auth(account);
      invalidations inv_table(get_self(), get_self().value);
      auto it = inv_table.find(account.value);
      if (it == inv_table.end())
      {
         inv_table.emplace(account, [&](auto &i) {
            i.account = account;
            i.last_invalidation_time = current_time_point();
         });
      }
      else
      {
         inv_table.modify(it, account, [&](auto &i) {
            i.last_invalidation_time = current_time_point();
         });
      }
   }

} // namespace hyphaspace
