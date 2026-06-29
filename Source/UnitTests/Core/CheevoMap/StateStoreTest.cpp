// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Core/CheevoMap/V2/StateStore.h"

namespace
{
using CheevoMap::V2::StateApplyStatus;
using CheevoMap::V2::StateStore;
using CheevoMap::V2::StateUpdate;
using CheevoMap::V2::StateValue;

TEST(CheevoMapStateStore, ResetPublishesFullSnapshot)
{
  StateStore store;
  std::vector<StateUpdate> updates;
  auto hook = store.RegisterUpdateCallback(
      [&updates](const StateUpdate& update) { updates.push_back(update); });
  (void)hook;

  const StateUpdate update = store.Reset({{"coins", StateValue::UnsignedInteger(3)}});

  ASSERT_EQ(updates.size(), 1u);
  EXPECT_TRUE(update.full);
  EXPECT_EQ(update.session_id, 1u);
  EXPECT_EQ(update.sequence, 1u);
  ASSERT_TRUE(updates.front().values.at("coins").AsUnsignedInteger());
  EXPECT_EQ(*updates.front().values.at("coins").AsUnsignedInteger(), 3u);

  const auto snapshot = store.GetSnapshot();
  EXPECT_EQ(snapshot.session_id, 1u);
  EXPECT_EQ(snapshot.sequence, 1u);
  ASSERT_TRUE(snapshot.values.at("coins").AsUnsignedInteger());
  EXPECT_EQ(*snapshot.values.at("coins").AsUnsignedInteger(), 3u);
}

TEST(CheevoMapStateStore, ApplyChangesPublishesOnlyChangedValues)
{
  StateStore store;
  store.Reset({{"coins", StateValue::UnsignedInteger(3)}, {"name", StateValue::String("Samus")}});

  std::vector<StateUpdate> updates;
  auto hook = store.RegisterUpdateCallback(
      [&updates](const StateUpdate& update) { updates.push_back(update); });
  (void)hook;

  auto update = store.ApplyChanges({{"coins", StateValue::UnsignedInteger(3)},
                                    {"health", StateValue::SignedInteger(-2)},
                                    {"name", StateValue::String("Link")}});

  ASSERT_TRUE(update);
  EXPECT_FALSE(update->full);
  EXPECT_EQ(update->sequence, 2u);
  EXPECT_FALSE(update->values.contains("coins"));
  ASSERT_TRUE(update->values.at("health").AsSignedInteger());
  EXPECT_EQ(*update->values.at("health").AsSignedInteger(), -2);
  ASSERT_NE(update->values.at("name").AsString(), nullptr);
  EXPECT_EQ(*update->values.at("name").AsString(), "Link");
  ASSERT_EQ(updates.size(), 1u);
}

TEST(CheevoMapStateStore, UnchangedUpdatesAreSuppressed)
{
  StateStore store;
  store.Reset({{"coins", StateValue::UnsignedInteger(3)}});

  std::vector<StateUpdate> updates;
  auto hook = store.RegisterUpdateCallback(
      [&updates](const StateUpdate& update) { updates.push_back(update); });
  (void)hook;

  EXPECT_FALSE(store.ApplyChanges({{"coins", StateValue::UnsignedInteger(3)}}));
  EXPECT_TRUE(updates.empty());
  EXPECT_EQ(store.GetSnapshot().sequence, 1u);
}

TEST(CheevoMapStateStore, CurrentSessionConditionalUpdatePublishesDelta)
{
  StateStore store;
  const StateUpdate reset = store.Reset({{"coins", StateValue::UnsignedInteger(3)}});

  std::vector<StateUpdate> updates;
  auto hook = store.RegisterUpdateCallback(
      [&updates](const StateUpdate& update) { updates.push_back(update); });
  (void)hook;

  const auto result =
      store.ApplyChangesForSession(reset.session_id, {{"coins", StateValue::UnsignedInteger(4)}});

  EXPECT_EQ(result.status, StateApplyStatus::Applied);
  ASSERT_TRUE(result.update);
  EXPECT_EQ(result.update->session_id, reset.session_id);
  EXPECT_EQ(result.update->sequence, 2u);
  ASSERT_TRUE(result.update->values.at("coins").AsUnsignedInteger());
  EXPECT_EQ(*result.update->values.at("coins").AsUnsignedInteger(), 4u);
  ASSERT_EQ(updates.size(), 1u);

  const auto snapshot = store.GetSnapshot();
  EXPECT_EQ(snapshot.session_id, reset.session_id);
  EXPECT_EQ(snapshot.sequence, 2u);
  ASSERT_TRUE(snapshot.values.at("coins").AsUnsignedInteger());
  EXPECT_EQ(*snapshot.values.at("coins").AsUnsignedInteger(), 4u);
}

TEST(CheevoMapStateStore, DeferredApplyDoesNotNotifyEarly)
{
  StateStore store;
  std::vector<StateUpdate> updates;
  auto hook = store.RegisterUpdateCallback(
      [&updates](const StateUpdate& update) { updates.push_back(update); });
  (void)hook;

  const StateUpdate reset = store.Reset({{"coins", StateValue::UnsignedInteger(3)}});
  updates.clear();

  const auto result = store.ApplyChangesForSessionDeferred(
      reset.session_id, {{"coins", StateValue::UnsignedInteger(4)}});

  EXPECT_EQ(result.status, StateApplyStatus::Applied);
  const auto snapshot = store.GetSnapshot();
  EXPECT_EQ(snapshot.sequence, 2u);
  ASSERT_TRUE(snapshot.values.at("coins").AsUnsignedInteger());
  EXPECT_EQ(*snapshot.values.at("coins").AsUnsignedInteger(), 4u);
  EXPECT_TRUE(updates.empty());

  store.DispatchPendingUpdates();

  ASSERT_EQ(updates.size(), 1u);
  EXPECT_EQ(updates.front().session_id, reset.session_id);
  EXPECT_EQ(updates.front().sequence, 2u);
}

TEST(CheevoMapStateStore, DeferredApplyIsDispatchedBeforeLifecycleReset)
{
  StateStore store;
  std::vector<StateUpdate> updates;
  auto hook = store.RegisterUpdateCallback(
      [&updates](const StateUpdate& update) { updates.push_back(update); });
  (void)hook;

  const StateUpdate session_a = store.Reset({{"coins", StateValue::Unavailable()}});
  updates.clear();

  const auto delta = store.ApplyChangesForSessionDeferred(
      session_a.session_id, {{"coins", StateValue::UnsignedInteger(7)}});
  ASSERT_EQ(delta.status, StateApplyStatus::Applied);

  const StateUpdate session_b = store.Reset({{"coins", StateValue::Unavailable()}});

  ASSERT_EQ(updates.size(), 2u);
  EXPECT_FALSE(updates[0].full);
  EXPECT_EQ(updates[0].session_id, session_a.session_id);
  ASSERT_TRUE(updates[0].values.at("coins").AsUnsignedInteger());
  EXPECT_EQ(*updates[0].values.at("coins").AsUnsignedInteger(), 7u);
  EXPECT_TRUE(updates[1].full);
  EXPECT_EQ(updates[1].session_id, session_b.session_id);

  store.DispatchPendingUpdates();
  ASSERT_EQ(updates.size(), 2u);
}

TEST(CheevoMapStateStore, ReentrantDispatchDeliversQueuedUpdateOnceAfterCurrentUpdate)
{
  StateStore store;
  store.Reset(
      {{"first", StateValue::UnsignedInteger(0)}, {"second", StateValue::UnsignedInteger(0)}});

  std::vector<StateUpdate> updates;
  auto hook = store.RegisterUpdateCallback([&store, &updates](const StateUpdate& update) {
    updates.push_back(update);
    if (updates.size() == 1)
      store.ApplyChanges({{"second", StateValue::UnsignedInteger(2)}});
  });
  (void)hook;

  const auto first = store.ApplyChanges({{"first", StateValue::UnsignedInteger(1)}});

  ASSERT_TRUE(first);
  ASSERT_EQ(updates.size(), 2u);
  ASSERT_TRUE(updates[0].values.contains("first"));
  EXPECT_FALSE(updates[0].values.contains("second"));
  ASSERT_TRUE(updates[1].values.contains("second"));
  EXPECT_FALSE(updates[1].values.contains("first"));
  EXPECT_EQ(updates[0].sequence, 2u);
  EXPECT_EQ(updates[1].sequence, 3u);
}

TEST(CheevoMapStateStore, ConditionalUpdateDistinguishesNoChangeFromStaleSession)
{
  StateStore store;
  const StateUpdate first = store.Reset({{"coins", StateValue::UnsignedInteger(3)}});

  std::vector<StateUpdate> updates;
  auto hook = store.RegisterUpdateCallback(
      [&updates](const StateUpdate& update) { updates.push_back(update); });
  (void)hook;

  const auto no_change =
      store.ApplyChangesForSession(first.session_id, {{"coins", StateValue::UnsignedInteger(3)}});
  EXPECT_EQ(no_change.status, StateApplyStatus::NoChanges);
  EXPECT_FALSE(no_change.update);
  EXPECT_TRUE(updates.empty());

  const StateUpdate second = store.Reset({{"coins", StateValue::UnsignedInteger(0)}});
  ASSERT_EQ(updates.size(), 1u);

  const auto stale =
      store.ApplyChangesForSession(first.session_id, {{"coins", StateValue::UnsignedInteger(9)}});
  EXPECT_EQ(stale.status, StateApplyStatus::StaleSession);
  EXPECT_FALSE(stale.update);
  ASSERT_EQ(updates.size(), 1u);

  const auto snapshot = store.GetSnapshot();
  EXPECT_EQ(snapshot.session_id, second.session_id);
  EXPECT_EQ(snapshot.sequence, 1u);
  ASSERT_TRUE(snapshot.values.at("coins").AsUnsignedInteger());
  EXPECT_EQ(*snapshot.values.at("coins").AsUnsignedInteger(), 0u);
}

TEST(CheevoMapStateStore, StaleUpdateAfterResetCannotRepopulateNewSession)
{
  StateStore store;
  const StateUpdate old_session = store.Reset({{"coins", StateValue::UnsignedInteger(3)}});

  std::vector<StateUpdate> updates;
  auto hook = store.RegisterUpdateCallback(
      [&updates](const StateUpdate& update) { updates.push_back(update); });
  (void)hook;

  const StateUpdate new_session = store.Reset({});
  ASSERT_EQ(updates.size(), 1u);

  const auto stale = store.ApplyChangesForSession(old_session.session_id,
                                                  {{"coins", StateValue::UnsignedInteger(99)}});

  EXPECT_EQ(stale.status, StateApplyStatus::StaleSession);
  EXPECT_FALSE(stale.update);
  ASSERT_EQ(updates.size(), 1u);
  EXPECT_EQ(updates.front().session_id, new_session.session_id);
  EXPECT_TRUE(store.GetSnapshot().values.empty());
  EXPECT_EQ(store.GetSnapshot().session_id, new_session.session_id);
}

TEST(CheevoMapStateStore, LifecycleCloseAndReloadRejectOlderEvaluationResults)
{
  StateStore store;
  const StateUpdate game_session = store.Reset({{"coins", StateValue::Unavailable()}});

  const StateUpdate close_session = store.Reset({});
  const auto stale_game_update = store.ApplyChangesForSession(
      game_session.session_id, {{"coins", StateValue::UnsignedInteger(99)}});
  EXPECT_EQ(stale_game_update.status, StateApplyStatus::StaleSession);
  EXPECT_EQ(store.GetSnapshot().session_id, close_session.session_id);
  EXPECT_TRUE(store.GetSnapshot().values.empty());

  const StateUpdate reload_session = store.Reset({{"coins", StateValue::Unavailable()}});
  const auto stale_close_update = store.ApplyChangesForSession(
      close_session.session_id, {{"coins", StateValue::UnsignedInteger(100)}});

  EXPECT_EQ(stale_close_update.status, StateApplyStatus::StaleSession);
  const auto snapshot = store.GetSnapshot();
  EXPECT_EQ(snapshot.session_id, reload_session.session_id);
  ASSERT_TRUE(snapshot.values.contains("coins"));
  EXPECT_FALSE(snapshot.values.at("coins").IsAvailable());
}

TEST(CheevoMapStateStore, RemovedValuesAreReported)
{
  StateStore store;
  store.Reset(
      {{"coins", StateValue::UnsignedInteger(3)}, {"health", StateValue::SignedInteger(2)}});

  const auto update = store.ApplyChanges({}, {"missing", "coins"});

  ASSERT_TRUE(update);
  ASSERT_EQ(update->removed.size(), 1u);
  EXPECT_EQ(update->removed.front(), "coins");
  EXPECT_FALSE(store.GetSnapshot().values.contains("coins"));
  EXPECT_TRUE(store.GetSnapshot().values.contains("health"));
}

TEST(CheevoMapStateStore, ResetStartsNewSession)
{
  StateStore store;
  store.Reset({{"coins", StateValue::UnsignedInteger(3)}});
  store.ApplyChanges({{"coins", StateValue::UnsignedInteger(4)}});

  const StateUpdate reset = store.Reset({{"coins", StateValue::UnsignedInteger(0)}});

  EXPECT_TRUE(reset.full);
  EXPECT_EQ(reset.session_id, 2u);
  EXPECT_EQ(reset.sequence, 1u);
  ASSERT_TRUE(store.GetSnapshot().values.at("coins").AsUnsignedInteger());
  EXPECT_EQ(*store.GetSnapshot().values.at("coins").AsUnsignedInteger(), 0u);
}

TEST(CheevoMapStateStore, MultipleListenersReceiveUpdates)
{
  StateStore store;
  int first_count = 0;
  int second_count = 0;
  auto first = store.RegisterUpdateCallback([&first_count](const StateUpdate&) { ++first_count; });
  auto second =
      store.RegisterUpdateCallback([&second_count](const StateUpdate&) { ++second_count; });
  (void)first;
  (void)second;

  store.Reset({});
  store.ApplyChanges({{"ready", StateValue::Boolean(true)}});

  EXPECT_EQ(first_count, 2);
  EXPECT_EQ(second_count, 2);
}

TEST(CheevoMapStateStore, DestroyedSubscriptionStopsCallbacks)
{
  StateStore store;
  int count = 0;
  {
    auto hook = store.RegisterUpdateCallback([&count](const StateUpdate&) { ++count; });
    (void)hook;
    store.Reset({});
  }

  store.ApplyChanges({{"ready", StateValue::Boolean(true)}});

  EXPECT_EQ(count, 1);
}

TEST(CheevoMapStateStore, ListenerCanReadSnapshotDuringCallback)
{
  StateStore store;
  bool saw_snapshot = false;
  auto hook = store.RegisterUpdateCallback([&store, &saw_snapshot](const StateUpdate& update) {
    const auto snapshot = store.GetSnapshot();
    saw_snapshot = snapshot.session_id == update.session_id && snapshot.sequence == update.sequence;
  });
  (void)hook;

  store.Reset({{"coins", StateValue::UnsignedInteger(3)}});

  EXPECT_TRUE(saw_snapshot);
}

TEST(CheevoMapStateStore, UnregisteringDuringCallbackSkipsRemovedListener)
{
  StateStore store;
  int first_count = 0;
  int second_count = 0;
  Common::EventHook second;

  auto first = store.RegisterUpdateCallback([&](const StateUpdate&) {
    ++first_count;
    second.reset();
  });
  second = store.RegisterUpdateCallback([&](const StateUpdate&) { ++second_count; });
  (void)first;

  store.Reset({});
  store.ApplyChanges({{"ready", StateValue::Boolean(true)}});

  EXPECT_EQ(first_count, 2);
  EXPECT_EQ(second_count, 0);
}

TEST(CheevoMapStateStore, ListenerCanRegisterDuringCallback)
{
  StateStore store;
  int first_count = 0;
  int late_count = 0;
  Common::EventHook late;

  auto first = store.RegisterUpdateCallback([&](const StateUpdate&) {
    ++first_count;
    if (!late)
      late = store.RegisterUpdateCallback([&](const StateUpdate&) { ++late_count; });
  });
  (void)first;

  store.Reset({});
  store.ApplyChanges({{"ready", StateValue::Boolean(true)}});

  EXPECT_EQ(first_count, 2);
  EXPECT_EQ(late_count, 2);
}

TEST(CheevoMapStateStore, FloatingPointValuesKeepFractions)
{
  StateStore store;
  store.Reset({{"timer", StateValue::FloatingPoint(12.75)}});

  const auto value = store.GetSnapshot().values.at("timer").AsFloatingPoint();
  ASSERT_TRUE(value);
  EXPECT_DOUBLE_EQ(*value, 12.75);
}
}  // namespace
