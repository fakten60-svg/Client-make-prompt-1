#include "jni/game_instance.h"

#include <jni.h>

#include <atomic>
#include <cstring>

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
constexpr const char* kGameOptionsClass = "class_315";  // GameOptions
constexpr const char* kSimpleOptionClass = "class_7172"; // SimpleOption
constexpr const char* kKeyBindingClass = "class_304";   // KeyBinding
constexpr const char* kEntityHitResultClass = "class_3966"; // EntityHitResult
constexpr const char* kTextClass = "class_2561";        // Text
constexpr const char* kItemStackClass = "class_1799";   // ItemStack
constexpr const char* kItemClass = "class_1792";        // Item

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

    // The option reads/writes (roadmap step 7). A SimpleOption holds its value as a boxed Object,
    // so a write boxes a Double through reflection_cache's java.lang bridge.
    jfieldID game_options_gamma = nullptr;
    jfieldID game_options_fov = nullptr;
    jmethodID simple_option_get_value = nullptr;
    jmethodID simple_option_set_value = nullptr;

    // The sprint key (roadmap step 8). The key's held state is a plain boolean on KeyBinding, so
    // unlike a SimpleOption write there is nothing to box.
    jfieldID game_options_sprint_key = nullptr;
    jfieldID game_options_sneak_key = nullptr;
    jmethodID key_binding_is_pressed = nullptr;
    jmethodID key_binding_set_pressed = nullptr;

    // Combat reads (roadmap step 8). Everything here is a read of the game's own state: the
    // crosshair target the game resolved, the entity's own health fields, and the player's own
    // cooldown/interaction numbers.
    jfieldID client_crosshair_target = nullptr;
    jmethodID hit_result_get_entity = nullptr;
    jmethodID entity_get_default_name = nullptr;
    jmethodID entity_distance_to = nullptr;
    jmethodID entity_is_alive = nullptr;
    jmethodID player_cooldown_progress = nullptr;
    // A method, not the ENTITY_INTERACTION_RANGE static field: the game exposes the range through
    // getEntityInteractionRange(), and reading a static field through GetFieldID would resolve to
    // nothing at all. The same reasoning the rest of the table follows.
    jmethodID player_entity_interaction_range = nullptr;
    jfieldID entity_fall_distance = nullptr;
    jmethodID text_as_truncated_string = nullptr;

    // Mace detection (roadmap step 8). The held item is read through the game's own stack, and the
    // item's translation key is the game's own answer to "which item is this" - no guessing at a
    // version-specific item comparison.
    jmethodID living_get_main_hand_stack = nullptr;
    jmethodID item_stack_get_item = nullptr;
    jmethodID item_get_translation_key = nullptr;

    // Spear reads (roadmap step 8). isUsingRiptide() is the game's own "a riptide is running" flag;
    // the held-item key (above) tells the client which weapon that is.
    jmethodID living_is_using_riptide = nullptr;
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

    take(g_handles.game_options_gamma, jni::field_of(kGameOptionsClass, "gamma"));
    take(g_handles.game_options_fov, jni::field_of(kGameOptionsClass, "fov"));
    take(g_handles.simple_option_get_value, jni::method_of(kSimpleOptionClass, "getValue"));
    take(g_handles.simple_option_set_value, jni::method_of(kSimpleOptionClass, "setValue"));

    take(g_handles.game_options_sprint_key, jni::field_of(kGameOptionsClass, "sprintKey"));
    take(g_handles.game_options_sneak_key, jni::field_of(kGameOptionsClass, "sneakKey"));
    take(g_handles.key_binding_is_pressed, jni::method_of(kKeyBindingClass, "isPressed"));
    take(g_handles.key_binding_set_pressed, jni::method_of(kKeyBindingClass, "setPressed"));

    take(g_handles.client_crosshair_target, jni::field_of(kClientClass, "crosshairTarget"));
    take(g_handles.hit_result_get_entity, jni::method_of(kEntityHitResultClass, "getEntity"));
    take(g_handles.entity_get_default_name, jni::method_of(kEntityClass, "getDefaultName"));
    take(g_handles.entity_distance_to, jni::method_of(kEntityClass, "distanceTo"));
    take(g_handles.entity_is_alive, jni::method_of(kEntityClass, "isAlive"));
    take(g_handles.player_cooldown_progress,
        jni::method_of(kPlayerClass, "getAttackCooldownProgress"));
    take(g_handles.player_entity_interaction_range,
        jni::method_of(kPlayerClass, "getEntityInteractionRange"));
    take(g_handles.entity_fall_distance, jni::field_of(kEntityClass, "fallDistance"));
    take(g_handles.text_as_truncated_string, jni::method_of(kTextClass, "asTruncatedString"));
    take(g_handles.living_get_main_hand_stack,
        jni::method_of(kLivingClass, "getMainHandStack"));
    take(g_handles.item_stack_get_item, jni::method_of(kItemStackClass, "getItem"));
    take(g_handles.item_get_translation_key,
        jni::method_of(kItemClass, "getTranslationKey"));
    take(g_handles.living_is_using_riptide, jni::method_of(kLivingClass, "isUsingRiptide"));
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

// client.options.<field> is a SimpleOption holding a boxed Double. One helper for both options
// keeps the read and the write symmetrical, so gamma and fov cannot drift apart.
[[nodiscard]] Maybe<float> read_option(jfieldID option_field) noexcept {
    if (!ready() || g_handles.client_options == nullptr || option_field == nullptr
        || g_handles.simple_option_get_value == nullptr) {
        return missing<float>();
    }

    JNIEnv* env = jni::current_env();
    jni::ScopedLocalFrame frame;
    jobject options = env->GetObjectField(client_ref(), g_handles.client_options);
    if (options == nullptr) {
        return missing<float>();
    }
    jobject option = env->GetObjectField(options, option_field);
    if (option == nullptr) {
        return missing<float>();
    }

    jobject boxed = env->CallObjectMethod(option, g_handles.simple_option_get_value);
    if (boxed == nullptr || !no_pending_exception(env)) {
        return missing<float>();
    }

    double value = 0.0;
    if (!jni::unbox_double(boxed, value)) {
        return missing<float>();
    }
    return Maybe<float>::of(static_cast<float>(value));
}

bool write_option(jfieldID option_field, float value) noexcept {
    if (!ready() || g_handles.client_options == nullptr || option_field == nullptr
        || g_handles.simple_option_set_value == nullptr) {
        return false;
    }

    JNIEnv* env = jni::current_env();
    jni::ScopedLocalFrame frame;
    jobject options = env->GetObjectField(client_ref(), g_handles.client_options);
    if (options == nullptr) {
        return false;
    }
    jobject option = env->GetObjectField(options, option_field);
    if (option == nullptr) {
        return false;
    }

    jobject boxed = jni::box_double(static_cast<double>(value));
    if (boxed == nullptr) {
        return false;
    }
    env->CallVoidMethod(option, g_handles.simple_option_set_value, boxed);
    return no_pending_exception(env);
}

// A KeyBinding object on the live GameOptions, addressed by its field id. Both the read and the
// write go through the binding the player's own keyboard drives, so the client's input handling
// sees exactly what it would see from the keyboard.
jobject option_key_binding(JNIEnv* env, jfieldID field) noexcept {
    if (g_handles.client_options == nullptr || field == nullptr) {
        return nullptr;
    }
    jobject options = env->GetObjectField(client_ref(), g_handles.client_options);
    if (options == nullptr) {
        return nullptr;
    }
    jobject binding = env->GetObjectField(options, field);
    if (binding == nullptr || !no_pending_exception(env)) {
        return nullptr;
    }
    return binding;
}

jobject sprint_binding(JNIEnv* env) noexcept {
    return option_key_binding(env, g_handles.game_options_sprint_key);
}

jobject sneak_binding(JNIEnv* env) noexcept {
    return option_key_binding(env, g_handles.game_options_sneak_key);
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

Maybe<float> gamma() noexcept {
    return read_option(g_handles.game_options_gamma);
}

Maybe<float> fov() noexcept {
    return read_option(g_handles.game_options_fov);
}

bool set_gamma(float value) noexcept {
    return write_option(g_handles.game_options_gamma, value);
}

bool set_fov(float degrees) noexcept {
    return write_option(g_handles.game_options_fov, degrees);
}

Maybe<bool> sprint_key_pressed() noexcept {
    if (!ready() || g_handles.key_binding_is_pressed == nullptr) {
        return missing<bool>();
    }

    JNIEnv* env = jni::current_env();
    jni::ScopedLocalFrame frame;
    jobject binding = sprint_binding(env);
    if (binding == nullptr) {
        return missing<bool>();
    }

    const jboolean pressed = env->CallBooleanMethod(binding, g_handles.key_binding_is_pressed);
    if (!no_pending_exception(env)) {
        return missing<bool>();
    }
    return Maybe<bool>::of(pressed != JNI_FALSE);
}

bool set_sprint_key_pressed(bool pressed) noexcept {
    if (!ready() || g_handles.key_binding_set_pressed == nullptr) {
        return false;
    }

    JNIEnv* env = jni::current_env();
    jni::ScopedLocalFrame frame;
    jobject binding = sprint_binding(env);
    if (binding == nullptr) {
        return false;
    }

    env->CallVoidMethod(binding, g_handles.key_binding_set_pressed, pressed ? JNI_TRUE : JNI_FALSE);
    return no_pending_exception(env);
}

// ── The sneak key (roadmap step 8: Safe Walk) ─────────────────────────────────────
//
// Exactly the sprint key's shape, on the GameOptions sneak binding: the game's own edge protection
// is what "safe walk" means, so holding its key is the whole feature.
Maybe<bool> sneak_key_pressed() noexcept {
    if (!ready() || g_handles.key_binding_is_pressed == nullptr) {
        return missing<bool>();
    }

    JNIEnv* env = jni::current_env();
    jni::ScopedLocalFrame frame;
    jobject binding = sneak_binding(env);
    if (binding == nullptr) {
        return missing<bool>();
    }

    const jboolean pressed = env->CallBooleanMethod(binding, g_handles.key_binding_is_pressed);
    if (!no_pending_exception(env)) {
        return missing<bool>();
    }
    return Maybe<bool>::of(pressed != JNI_FALSE);
}

bool set_sneak_key_pressed(bool pressed) noexcept {
    if (!ready() || g_handles.key_binding_set_pressed == nullptr) {
        return false;
    }

    JNIEnv* env = jni::current_env();
    jni::ScopedLocalFrame frame;
    jobject binding = sneak_binding(env);
    if (binding == nullptr) {
        return false;
    }

    env->CallVoidMethod(binding, g_handles.key_binding_set_pressed, pressed ? JNI_TRUE : JNI_FALSE);
    return no_pending_exception(env);
}

// ── Combat and mace reads (roadmap step 8) ───────────────────────────────────────

// Copies the entity's display name out of JNI before the local Text reference dies. A missing
// name is a zero-length buffer, never a crash: the HUD shows an unnamed plate.
void read_entity_name(JNIEnv* env, jobject entity, TargetInfo& out) noexcept {
    jobject name_text =
        env->CallObjectMethod(entity, g_handles.entity_get_default_name);
    if (name_text == nullptr || !no_pending_exception(env)
        || g_handles.text_as_truncated_string == nullptr) {
        return;
    }
    // 48 characters covers any nametag the HUD would print anyway; the chip truncates visually
    // at roughly the same length.
    jstring truncated = static_cast<jstring>(env->CallObjectMethod(
        name_text, g_handles.text_as_truncated_string, static_cast<jint>(48)));
    if (truncated == nullptr || !no_pending_exception(env)) {
        return;
    }
    const char* utf8 = env->GetStringUTFChars(truncated, nullptr);
    if (utf8 != nullptr) {
        out.name.assign(utf8);
        env->ReleaseStringUTFChars(truncated, utf8);
    }
}

Maybe<TargetInfo> crosshair_target() noexcept {
    if (!ready() || g_handles.client_crosshair_target == nullptr
        || g_handles.hit_result_get_entity == nullptr) {
        return missing<TargetInfo>();
    }

    JNIEnv* env = jni::current_env();
    jni::ScopedLocalFrame frame;
    jobject hit = env->GetObjectField(client_ref(), g_handles.client_crosshair_target);
    if (hit == nullptr || !no_pending_exception(env)) {
        return missing<TargetInfo>(); // no crosshair hit at all: a block, or the sky
    }

    // A crosshair hit may be a block, an entity or a miss. getEntity() exists only on
    // EntityHitResult, so the type is checked *before* the call: invoking a method id that the
    // instance's class does not declare is undefined, not a returned null.
    const jclass entity_hit_class = jni::class_of(kEntityHitResultClass);
    if (entity_hit_class == nullptr || env->IsInstanceOf(hit, entity_hit_class) != JNI_TRUE) {
        return missing<TargetInfo>();
    }

    jobject entity = env->CallObjectMethod(hit, g_handles.hit_result_get_entity);
    if (entity == nullptr || !no_pending_exception(env)) {
        return missing<TargetInfo>();
    }
    if (g_handles.entity_is_alive != nullptr) {
        const jboolean alive = env->CallBooleanMethod(entity, g_handles.entity_is_alive);
        if (!no_pending_exception(env) || alive == JNI_FALSE) {
            return missing<TargetInfo>();
        }
    }

    TargetInfo info{};

    // A LivingEntity answers health; anything else (armor stand, boat) has no such method, and
    // the read degrades to a name-and-distance card rather than a wrong number.
    if (g_handles.living_get_health != nullptr) {
        const jfloat health = env->CallFloatMethod(entity, g_handles.living_get_health);
        if (no_pending_exception(env)) {
            info.health = static_cast<float>(health);
        }
        const jfloat max_health = env->CallFloatMethod(entity, g_handles.living_get_max_health);
        if (no_pending_exception(env)) {
            info.max_health = static_cast<float>(max_health);
        }
        const jfloat absorption =
            env->CallFloatMethod(entity, g_handles.living_get_absorption);
        if (no_pending_exception(env)) {
            info.absorption = static_cast<float>(absorption);
        }
    }

    if (g_handles.entity_distance_to != nullptr) {
        jobject player = player_object(env);
        if (player != nullptr) {
            const jfloat distance =
                env->CallFloatMethod(entity, g_handles.entity_distance_to, player);
            if (no_pending_exception(env)) {
                info.distance = static_cast<float>(distance);
            }
        }
    }

    read_entity_name(env, entity, info);
    info.valid = true;
    return Maybe<TargetInfo>::of(info);
}

Maybe<float> attack_cooldown_progress() noexcept {
    if (!ready() || g_handles.player_cooldown_progress == nullptr) {
        return missing<float>();
    }

    JNIEnv* env = jni::current_env();
    jni::ScopedLocalFrame frame;
    jobject player = player_object(env);
    if (player == nullptr) {
        return missing<float>();
    }

    // (0.0f) reads "at the last completed tick", the same value the game's own HUD bar uses.
    const jfloat progress =
        env->CallFloatMethod(player, g_handles.player_cooldown_progress, 0.0f);
    if (!no_pending_exception(env)) {
        return missing<float>();
    }
    return Maybe<float>::of(static_cast<float>(progress));
}

Maybe<double> player_fall_distance() noexcept {
    if (!ready() || g_handles.entity_fall_distance == nullptr) {
        return missing<double>();
    }

    JNIEnv* env = jni::current_env();
    jni::ScopedLocalFrame frame;
    jobject player = player_object(env);
    if (player == nullptr) {
        return missing<double>();
    }

    const jdouble distance = env->GetDoubleField(player, g_handles.entity_fall_distance);
    if (!no_pending_exception(env) || distance < 0.0) {
        return missing<double>();
    }
    return Maybe<double>::of(static_cast<double>(distance));
}

Maybe<float> player_attack_range() noexcept {
    if (!ready() || g_handles.player_entity_interaction_range == nullptr) {
        return missing<float>();
    }

    JNIEnv* env = jni::current_env();
    jni::ScopedLocalFrame frame;
    jobject player = player_object(env);
    if (player == nullptr) {
        return missing<float>();
    }

    // getEntityInteractionRange() returns the game's own attack reach (the base 3.0 blocks plus
    // any attribute modifiers the player is carrying).
    const jdouble range =
        env->CallDoubleMethod(player, g_handles.player_entity_interaction_range);
    if (!no_pending_exception(env)) {
        return missing<float>();
    }
    return Maybe<float>::of(static_cast<float>(range));
}

// "Is the mace in my main hand?" answered by the game itself: the held stack's item translation
// key is compared to the mace's own key. A missing handle or a null stack is `false`, never a
// guess - the mace counters then simply stay at zero, which is the documented degradation.
Maybe<bool> holding_mace() noexcept {
    if (!ready() || g_handles.living_get_main_hand_stack == nullptr
        || g_handles.item_stack_get_item == nullptr
        || g_handles.item_get_translation_key == nullptr) {
        return missing<bool>();
    }

    JNIEnv* env = jni::current_env();
    jni::ScopedLocalFrame frame;
    jobject player = player_object(env);
    if (player == nullptr) {
        return missing<bool>();
    }

    jobject stack = env->CallObjectMethod(player, g_handles.living_get_main_hand_stack);
    if (stack == nullptr || !no_pending_exception(env)) {
        return Maybe<bool>::of(false); // an empty hand is not the mace
    }
    jobject item = env->CallObjectMethod(stack, g_handles.item_stack_get_item);
    if (item == nullptr || !no_pending_exception(env)) {
        return Maybe<bool>::of(false);
    }
    const auto key = static_cast<jstring>(
        env->CallObjectMethod(item, g_handles.item_get_translation_key));
    if (key == nullptr || !no_pending_exception(env)) {
        return Maybe<bool>::of(false);
    }

    const char* utf8 = env->GetStringUTFChars(key, nullptr);
    if (utf8 == nullptr) {
        return missing<bool>();
    }
    const bool is_mace = std::strcmp(utf8, "item.minecraft.mace") == 0;
    env->ReleaseStringUTFChars(key, utf8);
    return Maybe<bool>::of(is_mace);
}

// The held item's own translation key, e.g. "item.minecraft.trident". A bounded copy, so the caller
// compares against a known key instead of the client guessing at a version-specific item class.
Maybe<FixedName> held_item_key() noexcept {
    if (!ready() || g_handles.living_get_main_hand_stack == nullptr
        || g_handles.item_stack_get_item == nullptr
        || g_handles.item_get_translation_key == nullptr) {
        return missing<FixedName>();
    }

    JNIEnv* env = jni::current_env();
    jni::ScopedLocalFrame frame;
    jobject player = player_object(env);
    if (player == nullptr) {
        return missing<FixedName>();
    }
    jobject stack = env->CallObjectMethod(player, g_handles.living_get_main_hand_stack);
    if (stack == nullptr || !no_pending_exception(env)) {
        // An empty hand is a *valid* answer with an empty key: the caller distinguishes "not holding
        // the item" from "the read is unavailable", which matters for the loyalty observation.
        return Maybe<FixedName>::of(FixedName{});
    }
    jobject item = env->CallObjectMethod(stack, g_handles.item_stack_get_item);
    if (item == nullptr || !no_pending_exception(env)) {
        return missing<FixedName>();
    }
    const auto key = static_cast<jstring>(
        env->CallObjectMethod(item, g_handles.item_get_translation_key));
    if (key == nullptr || !no_pending_exception(env)) {
        return missing<FixedName>();
    }

    const char* utf8 = env->GetStringUTFChars(key, nullptr);
    if (utf8 == nullptr) {
        return missing<FixedName>();
    }
    FixedName name;
    name.assign(utf8);
    env->ReleaseStringUTFChars(key, utf8);
    return Maybe<FixedName>::of(name);
}

// The game's own "a riptide is in progress" flag. Read-only, and the trident-held check is the
// caller's (through held_item_key), so this stays one fact about the player.
Maybe<bool> riptide_active() noexcept {
    if (!ready() || g_handles.living_is_using_riptide == nullptr) {
        return missing<bool>();
    }

    JNIEnv* env = jni::current_env();
    jni::ScopedLocalFrame frame;
    jobject player = player_object(env);
    if (player == nullptr) {
        return missing<bool>();
    }

    const jboolean active = env->CallBooleanMethod(player, g_handles.living_is_using_riptide);
    if (!no_pending_exception(env)) {
        return missing<bool>();
    }
    return Maybe<bool>::of(active != JNI_FALSE);
}

} // namespace woke::game
