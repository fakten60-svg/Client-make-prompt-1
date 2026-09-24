#include "jni/game_instance.h"

#include <jni.h>

#include <atomic>

#include "core/logger.h"
#include "jni/jni_context.h"
#include "jni/reflection_cache.h"

namespace woke::game {
namespace {

// Declaring classes, addressed by their intermediary aliases from mappings.json. A member
// must be looked up on the class that declares it; JNI happily calls an inherited method
// through the superclass id, which is why Entity/LivingEntity appear here directly.
constexpr const char* kClientClass = "class_310";       // MinecraftClient
constexpr const char* kEntityClass = "class_1297";      // Entity
constexpr const char* kLivingClass = "class_1309";      // LivingEntity
constexpr const char* kPlayerClass = "class_1657";      // PlayerEntity
constexpr const char* kInventoryClass = "class_1661";   // PlayerInventory
constexpr const char* kWorldClass = "class_638";        // ClientWorld
constexpr const char* kMouseClass = "class_312";        // Mouse
constexpr const char* kVec3dClass = "class_243";        // Vec3d

struct Handles {
    jclass client_class = nullptr;
    jmethodID client_get_instance = nullptr;
    jfieldID client_player = nullptr;
    jfieldID client_world = nullptr;
    jfieldID client_options = nullptr;
    jfieldID client_mouse = nullptr;
    jfieldID client_in_game_hud = nullptr;

    jmethodID entity_get_x = nullptr;
    jmethodID entity_get_y = nullptr;
    jmethodID entity_get_z = nullptr;
    jmethodID entity_get_yaw = nullptr;
    jmethodID entity_get_pitch = nullptr;
    jmethodID entity_get_velocity = nullptr;
    jmethodID entity_get_eye_pos = nullptr;
    jmethodID entity_is_on_ground = nullptr;

    jfieldID vec3d_x = nullptr;
    jfieldID vec3d_y = nullptr;
    jfieldID vec3d_z = nullptr;

    jmethodID living_get_health = nullptr;
    jmethodID living_get_max_health = nullptr;
    jmethodID living_get_absorption = nullptr;

    jfieldID player_inventory = nullptr;
    jmethodID inventory_get_selected_slot = nullptr;

    jmethodID world_entity_count = nullptr;

    jmethodID mouse_get_x = nullptr;
    jmethodID mouse_get_y = nullptr;
};

Handles g_handles{};

// Global ref to the live MinecraftClient.
//
// Atomic because roadmap step 3 puts a second reader on the game thread: the worker thread
// resolves and publishes the reference, and every frame afterwards reads it from the render
// thread. The release/acquire pair is what makes "a visible client reference implies a fully
// populated handle table" an actual guarantee instead of an ordering coincidence.
std::atomic<jobject> g_client{nullptr};
std::size_t g_unresolved = 0;

[[nodiscard]] jobject client_ref() noexcept {
    return g_client.load(std::memory_order_acquire);
}

// Records a resolution outcome in one line per member, so the handle table stays readable
// instead of becoming a wall of conditionals.
template <typename Handle>
void take(Handle& target, Handle resolved) noexcept {
    target = resolved;
    if (resolved == nullptr) {
        ++g_unresolved;
    }
}

void resolve_handles() noexcept {
    g_unresolved = 0;
    g_handles = Handles{};

    g_handles.client_class = jni::class_of(kClientClass);
    if (g_handles.client_class == nullptr) {
        ++g_unresolved;
        return;
    }

    take(g_handles.client_get_instance, jni::static_method_of(kClientClass, "getInstance"));
    take(g_handles.client_player, jni::field_of(kClientClass, "player"));
    take(g_handles.client_world, jni::field_of(kClientClass, "world"));
    take(g_handles.client_options, jni::field_of(kClientClass, "options"));
    take(g_handles.client_mouse, jni::field_of(kClientClass, "mouse"));
    take(g_handles.client_in_game_hud, jni::field_of(kClientClass, "inGameHud"));

    take(g_handles.entity_get_x, jni::method_of(kEntityClass, "getX"));
    take(g_handles.entity_get_y, jni::method_of(kEntityClass, "getY"));
    take(g_handles.entity_get_z, jni::method_of(kEntityClass, "getZ"));
    take(g_handles.entity_get_yaw, jni::method_of(kEntityClass, "getYaw"));
    take(g_handles.entity_get_pitch, jni::method_of(kEntityClass, "getPitch"));
    take(g_handles.entity_get_velocity, jni::method_of(kEntityClass, "getVelocity"));
    take(g_handles.entity_get_eye_pos, jni::method_of(kEntityClass, "getEyePos"));
    take(g_handles.entity_is_on_ground, jni::method_of(kEntityClass, "isOnGround"));

    take(g_handles.vec3d_x, jni::field_of(kVec3dClass, "x"));
    take(g_handles.vec3d_y, jni::field_of(kVec3dClass, "y"));
    take(g_handles.vec3d_z, jni::field_of(kVec3dClass, "z"));

    take(g_handles.living_get_health, jni::method_of(kLivingClass, "getHealth"));
    take(g_handles.living_get_max_health, jni::method_of(kLivingClass, "getMaxHealth"));
    take(g_handles.living_get_absorption, jni::method_of(kLivingClass, "getAbsorptionAmount"));

    take(g_handles.player_inventory, jni::field_of(kPlayerClass, "inventory"));
    take(g_handles.inventory_get_selected_slot,
        jni::method_of(kInventoryClass, "getSelectedSlot"));

    take(g_handles.world_entity_count, jni::method_of(kWorldClass, "getRegularEntityCount"));

    take(g_handles.mouse_get_x, jni::method_of(kMouseClass, "getX"));
    take(g_handles.mouse_get_y, jni::method_of(kMouseClass, "getY"));
}

// True when the last call left no pending exception. A pending exception would poison every
// later JNI call on this thread, so it is always cleared here.
bool no_pending_exception(JNIEnv* env) noexcept {
    if (env->ExceptionCheck() == JNI_FALSE) {
        return true;
    }
    env->ExceptionClear();
    WOKE_LOG_DEBUG("game: a read raised a JVM exception and was discarded");
    return false;
}

jobject player_object(JNIEnv* env) noexcept {
    jobject client = client_ref();
    if (client == nullptr || g_handles.client_player == nullptr) {
        return nullptr;
    }
    return env->GetObjectField(client, g_handles.client_player);
}

template <typename T>
Maybe<T> missing() noexcept {
    return Maybe<T>::missing();
}

bool ready() noexcept {
    return client_ref() != nullptr && jni::current_env() != nullptr;
}

} // namespace

bool initialize() noexcept {
    if (client_ref() != nullptr) {
        return true;
    }
    if (!jni::available()) {
        return false;
    }

    JNIEnv* env = jni::current_env();
    if (env == nullptr) {
        return false;
    }

    jni::ScopedLocalFrame frame;
    resolve_handles();

    if (g_handles.client_class == nullptr || g_handles.client_get_instance == nullptr) {
        WOKE_LOG_ERROR("game: MinecraftClient is unreachable - JNI modules stay disabled");
        return false;
    }

    jobject instance = env->CallStaticObjectMethod(g_handles.client_class, g_handles.client_get_instance);
    if (instance == nullptr) {
        (void)no_pending_exception(env);
        WOKE_LOG_WARN("game: MinecraftClient has no live instance yet - will retry");
        return false;
    }

    jobject reference = env->NewGlobalRef(instance);
    if (reference == nullptr) {
        WOKE_LOG_ERROR("game: NewGlobalRef failed for the MinecraftClient instance");
        return false;
    }
    // Published last: everything the accessors need is resolved by this point.
    g_client.store(reference, std::memory_order_release);

    const jni::CacheStats cache = jni::stats();
    WOKE_LOG_INFO("game: MinecraftClient cached (%zu handle(s) unresolved, cache %zu/%zu/%zu)",
        g_unresolved, cache.classes, cache.methods, cache.fields);
    WOKE_LOG_INFO("game: %s", in_world() ? "world and player are live" : "no world loaded yet");
    return true;
}

void shutdown() noexcept {
    JNIEnv* env = jni::current_env();
    jobject reference = g_client.exchange(nullptr, std::memory_order_acq_rel);
    if (env != nullptr && reference != nullptr) {
        env->DeleteGlobalRef(reference);
    }
    g_handles = Handles{};
    g_unresolved = 0;
}

bool available() noexcept {
    return client_ref() != nullptr;
}

bool has_player() noexcept {
    if (!ready() || g_handles.client_player == nullptr) {
        return false;
    }
    JNIEnv* env = jni::current_env();
    jni::ScopedLocalFrame frame;
    return player_object(env) != nullptr;
}

bool in_world() noexcept {
    if (!ready()) {
        return false;
    }
    if (g_handles.client_player == nullptr || g_handles.client_world == nullptr) {
        return false;
    }

    jobject client = client_ref();
    JNIEnv* env = jni::current_env();
    jni::ScopedLocalFrame frame;
    jobject player = env->GetObjectField(client, g_handles.client_player);
    jobject world = env->GetObjectField(client, g_handles.client_world);
    return player != nullptr && world != nullptr;
}

std::size_t unresolved_handle_count() noexcept {
    return g_unresolved;
}

Maybe<Vec3d> player_position() noexcept {
    if (!ready() || g_handles.entity_get_x == nullptr || g_handles.entity_get_y == nullptr
        || g_handles.entity_get_z == nullptr) {
        return missing<Vec3d>();
    }

    JNIEnv* env = jni::current_env();
    jni::ScopedLocalFrame frame;
    jobject player = player_object(env);
    if (player == nullptr) {
        return missing<Vec3d>();
    }

    Vec3d position;
    position.x = static_cast<double>(env->CallDoubleMethod(player, g_handles.entity_get_x));
    position.y = static_cast<double>(env->CallDoubleMethod(player, g_handles.entity_get_y));
    position.z = static_cast<double>(env->CallDoubleMethod(player, g_handles.entity_get_z));
    if (!no_pending_exception(env)) {
        return missing<Vec3d>();
    }
    return Maybe<Vec3d>::of(position);
}

Maybe<Vec3d> player_velocity() noexcept {
    if (!ready() || g_handles.entity_get_velocity == nullptr || g_handles.vec3d_x == nullptr
        || g_handles.vec3d_y == nullptr || g_handles.vec3d_z == nullptr) {
        return missing<Vec3d>();
    }

    JNIEnv* env = jni::current_env();
    jni::ScopedLocalFrame frame;
    jobject player = player_object(env);
    if (player == nullptr) {
        return missing<Vec3d>();
    }

    jobject velocity = env->CallObjectMethod(player, g_handles.entity_get_velocity);
    if (velocity == nullptr || !no_pending_exception(env)) {
        return missing<Vec3d>();
    }

    Vec3d result;
    result.x = static_cast<double>(env->GetDoubleField(velocity, g_handles.vec3d_x));
    result.y = static_cast<double>(env->GetDoubleField(velocity, g_handles.vec3d_y));
    result.z = static_cast<double>(env->GetDoubleField(velocity, g_handles.vec3d_z));
    if (!no_pending_exception(env)) {
        return missing<Vec3d>();
    }
    return Maybe<Vec3d>::of(result);
}

Maybe<Vec3d> player_eye_position() noexcept {
    if (!ready() || g_handles.entity_get_eye_pos == nullptr || g_handles.vec3d_x == nullptr
        || g_handles.vec3d_y == nullptr || g_handles.vec3d_z == nullptr) {
        return missing<Vec3d>();
    }

    JNIEnv* env = jni::current_env();
    jni::ScopedLocalFrame frame;
    jobject player = player_object(env);
    if (player == nullptr) {
        return missing<Vec3d>();
    }

    jobject eye = env->CallObjectMethod(player, g_handles.entity_get_eye_pos);
    if (eye == nullptr || !no_pending_exception(env)) {
        return missing<Vec3d>();
    }

    Vec3d result;
    result.x = static_cast<double>(env->GetDoubleField(eye, g_handles.vec3d_x));
    result.y = static_cast<double>(env->GetDoubleField(eye, g_handles.vec3d_y));
    result.z = static_cast<double>(env->GetDoubleField(eye, g_handles.vec3d_z));
    if (!no_pending_exception(env)) {
        return missing<Vec3d>();
    }
    return Maybe<Vec3d>::of(result);
}

Maybe<float> player_yaw() noexcept {
    if (!ready() || g_handles.entity_get_yaw == nullptr) {
        return missing<float>();
    }

    JNIEnv* env = jni::current_env();
    jni::ScopedLocalFrame frame;
    jobject player = player_object(env);
    if (player == nullptr) {
        return missing<float>();
    }

    // getYaw(float partialTick) - the render partial tick is 0.0 for a state read.
    const jfloat yaw = env->CallFloatMethod(player, g_handles.entity_get_yaw, 0.0f);
    if (!no_pending_exception(env)) {
        return missing<float>();
    }
    return Maybe<float>::of(static_cast<float>(yaw));
}

Maybe<float> player_pitch() noexcept {
    if (!ready() || g_handles.entity_get_pitch == nullptr) {
        return missing<float>();
    }

    JNIEnv* env = jni::current_env();
    jni::ScopedLocalFrame frame;
    jobject player = player_object(env);
    if (player == nullptr) {
        return missing<float>();
    }

    const jfloat pitch = env->CallFloatMethod(player, g_handles.entity_get_pitch);
    if (!no_pending_exception(env)) {
        return missing<float>();
    }
    return Maybe<float>::of(static_cast<float>(pitch));
}

Maybe<float> player_health() noexcept {
    if (!ready() || g_handles.living_get_health == nullptr) {
        return missing<float>();
    }

    JNIEnv* env = jni::current_env();
    jni::ScopedLocalFrame frame;
    jobject player = player_object(env);
    if (player == nullptr) {
        return missing<float>();
    }

    const jfloat health = env->CallFloatMethod(player, g_handles.living_get_health);
    if (!no_pending_exception(env)) {
        return missing<float>();
    }
    return Maybe<float>::of(static_cast<float>(health));
}

Maybe<float> player_max_health() noexcept {
    if (!ready() || g_handles.living_get_max_health == nullptr) {
        return missing<float>();
    }

    JNIEnv* env = jni::current_env();
    jni::ScopedLocalFrame frame;
    jobject player = player_object(env);
    if (player == nullptr) {
        return missing<float>();
    }

    const jfloat health = env->CallFloatMethod(player, g_handles.living_get_max_health);
    if (!no_pending_exception(env)) {
        return missing<float>();
    }
    return Maybe<float>::of(static_cast<float>(health));
}

Maybe<float> player_absorption() noexcept {
    if (!ready() || g_handles.living_get_absorption == nullptr) {
        return missing<float>();
    }

    JNIEnv* env = jni::current_env();
    jni::ScopedLocalFrame frame;
    jobject player = player_object(env);
    if (player == nullptr) {
        return missing<float>();
    }

    const jfloat absorption = env->CallFloatMethod(player, g_handles.living_get_absorption);
    if (!no_pending_exception(env)) {
        return missing<float>();
    }
    return Maybe<float>::of(static_cast<float>(absorption));
}

Maybe<bool> player_on_ground() noexcept {
    if (!ready() || g_handles.entity_is_on_ground == nullptr) {
        return missing<bool>();
    }

    JNIEnv* env = jni::current_env();
    jni::ScopedLocalFrame frame;
    jobject player = player_object(env);
    if (player == nullptr) {
        return missing<bool>();
    }

    const jboolean on_ground = env->CallBooleanMethod(player, g_handles.entity_is_on_ground);
    if (!no_pending_exception(env)) {
        return missing<bool>();
    }
    return Maybe<bool>::of(on_ground != JNI_FALSE);
}

Maybe<int> selected_hotbar_slot() noexcept {
    if (!ready() || g_handles.player_inventory == nullptr
        || g_handles.inventory_get_selected_slot == nullptr) {
        return missing<int>();
    }

    JNIEnv* env = jni::current_env();
    jni::ScopedLocalFrame frame;
    jobject player = player_object(env);
    if (player == nullptr) {
        return missing<int>();
    }

    jobject inventory = env->GetObjectField(player, g_handles.player_inventory);
    if (inventory == nullptr || !no_pending_exception(env)) {
        return missing<int>();
    }

    const jint slot = env->CallIntMethod(inventory, g_handles.inventory_get_selected_slot);
    if (!no_pending_exception(env)) {
        return missing<int>();
    }
    return Maybe<int>::of(static_cast<int>(slot));
}

Maybe<int> world_entity_count() noexcept {
    if (!ready() || g_handles.client_world == nullptr || g_handles.world_entity_count == nullptr) {
        return missing<int>();
    }

    JNIEnv* env = jni::current_env();
    jni::ScopedLocalFrame frame;
    jobject world = env->GetObjectField(client_ref(), g_handles.client_world);
    if (world == nullptr) {
        return missing<int>();
    }

    const jint count = env->CallIntMethod(world, g_handles.world_entity_count);
    if (!no_pending_exception(env)) {
        return missing<int>();
    }
    return Maybe<int>::of(static_cast<int>(count));
}

Maybe<double> mouse_x() noexcept {
    if (!ready() || g_handles.client_mouse == nullptr || g_handles.mouse_get_x == nullptr) {
        return missing<double>();
    }

    JNIEnv* env = jni::current_env();
    jni::ScopedLocalFrame frame;
    jobject mouse = env->GetObjectField(client_ref(), g_handles.client_mouse);
    if (mouse == nullptr) {
        return missing<double>();
    }

    const jdouble x = env->CallDoubleMethod(mouse, g_handles.mouse_get_x);
    if (!no_pending_exception(env)) {
        return missing<double>();
    }
    return Maybe<double>::of(static_cast<double>(x));
}

Maybe<double> mouse_y() noexcept {
    if (!ready() || g_handles.client_mouse == nullptr || g_handles.mouse_get_y == nullptr) {
        return missing<double>();
    }

    JNIEnv* env = jni::current_env();
    jni::ScopedLocalFrame frame;
    jobject mouse = env->GetObjectField(client_ref(), g_handles.client_mouse);
    if (mouse == nullptr) {
        return missing<double>();
    }

    const jdouble y = env->CallDoubleMethod(mouse, g_handles.mouse_get_y);
    if (!no_pending_exception(env)) {
        return missing<double>();
    }
    return Maybe<double>::of(static_cast<double>(y));
}

} // namespace woke::game
