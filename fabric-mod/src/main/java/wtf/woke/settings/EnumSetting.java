package wtf.woke.settings;

import com.google.gson.JsonElement;
import com.google.gson.JsonPrimitive;

/** A fixed list of labeled options; the GUI cycles through them and the config stores the label. */
public final class EnumSetting extends Setting {
    private final String[] labels;
    private int index;

    public EnumSetting(String name, String description, String[] labels, int defaultIndex) {
        super(name, description);
        this.labels = labels.clone();
        this.index = Math.floorMod(defaultIndex, this.labels.length);
    }

    public int count() {
        return labels.length;
    }

    public int index() {
        return index;
    }

    public String label() {
        return labels[index];
    }

    public String label(int at) {
        return labels[at];
    }

    public void set(int requested) {
        int next = Math.floorMod(requested, labels.length);
        if (next != index) {
            index = next;
            markDirty();
        }
    }

    public void cycle() {
        set(index + 1);
    }

    @Override
    public JsonElement toJson() {
        return new JsonPrimitive(labels[index]);
    }

    @Override
    public void fromJson(JsonElement element) {
        if (element != null && element.isJsonPrimitive() && element.getAsJsonPrimitive().isString()) {
            String label = element.getAsString();
            for (int i = 0; i < labels.length; i++) {
                if (labels[i].equals(label)) {
                    set(i);
                    return;
                }
            }
        }
    }
}
