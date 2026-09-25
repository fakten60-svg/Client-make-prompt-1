package wtf.woke.module;

import java.util.ArrayList;
import java.util.List;

import net.minecraft.client.MinecraftClient;

import wtf.woke.settings.Setting;

/**
 * Base class for every module. Port of the C++ client's BaseModule contract:
 *
 * <ul>
 *   <li>a module owns its settings by declaration, and persistence is automatic;</li>
 *   <li>{@link #setEnabled} is the only mutation path, and subclasses implement
 *       {@link #onEnable}/{@link #onDisable} which are called only after the state flag has
 *       flipped — a hook can never observe a "disabled module that thinks it is enabled";</li>
 *   <li>constructors never touch the live game: all game interaction happens in
 *       {@link #tick} through the client handed in by the manager.</li>
 * </ul>
 *
 * All modules here operate strictly through client-side game state with visible UI toggles —
 * no packet synthesis and no hidden background actions, by design.
 */
public abstract class Module {
    private final String id;
    private final String name;
    private final String description;
    private final Category category;
    private final List<Setting> settings = new ArrayList<>();

    /** GLFW keycode that toggles/holds this module; 0 = unbound. */
    private int bind;
    /** True when the module polls its own bind every tick (hold-style, e.g. zoom). */
    private final boolean holdKey;
    private boolean enabled;
    /** Set when the enabled state changed since the last config save. */
    private boolean stateDirty;
    /** Rising-edge detector state for the bind poll. */
    private boolean bindWasDown;

    protected Module(String id, String name, String description, Category category,
                     int defaultBind, boolean holdKey) {
        this.id = id;
        this.name = name;
        this.description = description;
        this.category = category;
        this.bind = defaultBind;
        this.holdKey = holdKey;
    }

    public String id() {
        return id;
    }

    public String name() {
        return name;
    }

    public String description() {
        return description;
    }

    public Category category() {
        return category;
    }

    public List<Setting> settings() {
        return settings;
    }

    public int bind() {
        return bind;
    }

    public void setBind(int keycode) {
        if (keycode < 0) {
            keycode = 0;
        }
        if (keycode != bind) {
            bind = keycode;
            stateDirty = true;
        }
    }

    public boolean holdKey() {
        return holdKey;
    }

    public boolean enabled() {
        return enabled;
    }

    public boolean dirty() {
        if (stateDirty) {
            return true;
        }
        for (Setting setting : settings) {
            if (setting.dirty()) {
                return true;
            }
        }
        return false;
    }

    public void clearDirty() {
        stateDirty = false;
        for (Setting setting : settings) {
            setting.clearDirty();
        }
    }

    /** The only mutation path. Returns true when the state actually changed. */
    public boolean setEnabled(boolean requested) {
        if (requested == enabled) {
            return false;
        }
        if (requested) {
            enabled = true;
            if (!onEnable()) {
                enabled = false;
                return false;
            }
        } else {
            enabled = false;
            onDisable();
        }
        stateDirty = true;
        return true;
    }

    /**
     * Per-tick update. Runs only while enabled, on the client thread, with the live client
     * handed in — modules read and write client-side game state through it.
     */
    public void tick(MinecraftClient client) {
    }

    /** Rising-edge detector shared by the manager's bind poll. */
    boolean pollBind(boolean down) {
        boolean pressed = down && !bindWasDown;
        bindWasDown = down;
        return pressed;
    }

    /** Returning false refuses the enable (a prerequisite is missing). */
    protected boolean onEnable() {
        return true;
    }

    protected abstract void onDisable();

    protected final void add(Setting setting) {
        settings.add(setting);
    }
}
