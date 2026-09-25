package wtf.woke.settings;

import com.google.gson.JsonElement;
import com.google.gson.JsonPrimitive;

/** A named on/off value. */
public final class BooleanSetting extends Setting {
    private boolean value;

    public BooleanSetting(String name, String description, boolean defaultValue) {
        super(name, description);
        this.value = defaultValue;
    }

    public boolean value() {
        return value;
    }

    public void set(boolean requested) {
        if (requested != value) {
            value = requested;
            markDirty();
        }
    }

    public void toggle() {
        set(!value);
    }

    @Override
    public JsonElement toJson() {
        return new JsonPrimitive(value);
    }

    @Override
    public void fromJson(JsonElement element) {
        if (element != null && element.isJsonPrimitive() && element.getAsJsonPrimitive().isBoolean()) {
            set(element.getAsBoolean());
        }
    }
}
