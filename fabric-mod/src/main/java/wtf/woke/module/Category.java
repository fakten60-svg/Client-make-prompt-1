package wtf.woke.module;

/**
 * The module categories. Mirrors the C++ client's six-category registry so the GUI layout and
 * future module sets carry over unchanged. Only Visual and Misc are populated by the QoL set.
 */
public enum Category {
    Combat("Combat"),
    Mace("Mace"),
    Misc("Misc"),
    Movement("Movement"),
    Spear("Spear"),
    Visual("Visual");

    private final String label;

    Category(String label) {
        this.label = label;
    }

    public String label() {
        return label;
    }
}
