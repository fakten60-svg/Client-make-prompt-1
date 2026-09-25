package wtf.woke.settings;

import com.google.gson.JsonElement;
import com.google.gson.JsonPrimitive;

/**
 * A clamped numeric value with a step. Values are rounded to the step on every write so slider
 * churn cannot fill the config file with float noise.
 */
public final class SliderSetting extends Setting {
    private final double minimum;
    private final double maximum;
    private final double step;
    private double value;

    public SliderSetting(String name, String description, double defaultValue,
                         double minimum, double maximum, double step) {
        super(name, description);
        this.minimum = minimum;
        this.maximum = maximum;
        this.step = step;
        this.value = clamp(defaultValue);
    }

    public double value() {
        return value;
    }

    public double minimum() {
        return minimum;
    }

    public double maximum() {
        return maximum;
    }

    public double step() {
        return step;
    }

    public void set(double requested) {
        double rounded = Math.round(requested / step) * step;
        double clamped = clamp(rounded);
        if (clamped != value) {
            value = clamped;
            markDirty();
        }
    }

    private double clamp(double v) {
        return Math.max(minimum, Math.min(maximum, v));
    }

    @Override
    public JsonElement toJson() {
        return new JsonPrimitive(Math.round(value * 1000.0) / 1000.0);
    }

    @Override
    public void fromJson(JsonElement element) {
        if (element != null && element.isJsonPrimitive() && element.getAsJsonPrimitive().isNumber()) {
            set(element.getAsDouble());
        }
    }
}
