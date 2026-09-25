package wtf.woke.settings;

import com.google.gson.JsonElement;

/**
 * Base class for every module setting.
 *
 * <p>Port of the C++ client's auto-binding invariant: a setting knows how to move itself to and
 * from JSON, so the config engine walks a module's settings and needs zero per-module persistence
 * code. Adding a module with new settings requires no config wiring — persistence is a property of
 * the setting itself.
 *
 * <p>Tolerance contract: a malformed or wrong-typed JSON value is ignored (the default survives)
 * and never throws; persistence problems must never take the game down.
 */
public abstract class Setting {
    private final String name;
    private final String description;
    private boolean dirty;

    protected Setting(String name, String description) {
        this.name = name;
        this.description = description;
    }

    public String name() {
        return name;
    }

    public String description() {
        return description;
    }

    public boolean dirty() {
        return dirty;
    }

    public void clearDirty() {
        dirty = false;
    }

    /** Serializes the live value. */
    public abstract JsonElement toJson();

    /** Applies a JSON value only when it is present and of the right shape. */
    public abstract void fromJson(JsonElement element);

    protected void markDirty() {
        dirty = true;
    }
}
