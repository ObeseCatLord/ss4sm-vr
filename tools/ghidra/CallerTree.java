// Headless: print the caller tree (direct call references, incl. vtable/data pointers) of the functions
// containing each listed address, up to a depth. Read-only.
// args: <addresses file (hex per line, optional name)> <output file> [depth=4]
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import java.io.*;
import java.nio.file.*;
import java.util.*;

public class CallerTree extends GhidraScript {
    PrintWriter w;
    int maxDepth;

    void walk(Function f, int depth, Set<Function> path) {
        if (depth > maxDepth) return;
        for (Reference r : getReferencesTo(f.getEntryPoint())) {
            Address from = r.getFromAddress();
            Function c = getFunctionContaining(from);
            String ind = "  ".repeat(depth);
            if (c == null) {
                w.println(ind + "<- " + r.getReferenceType() + " from " + from + " (no function: data/vtable)");
                continue;
            }
            w.println(ind + "<- " + c.getEntryPoint() + " " + c.getName() + " at " + from + " " + r.getReferenceType());
            if (path.add(c)) {
                walk(c, depth + 1, path);
                path.remove(c);
            }
        }
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        maxDepth = args.length > 2 ? Integer.parseInt(args[2]) : 4;
        List<String> lines = Files.readAllLines(Paths.get(args[0]));
        try (PrintWriter pw = new PrintWriter(new FileWriter(args[1]))) {
            w = pw;
            for (String line : lines) {
                line = line.trim();
                if (line.isEmpty() || line.startsWith("#")) continue;
                String[] parts = line.split("\\s+", 2);
                Address a = toAddr(Long.parseUnsignedLong(parts[0].replace("0x", ""), 16));
                Function f = getFunctionContaining(a);
                if (f == null) { w.println("// no function at " + parts[0]); continue; }
                w.println("== " + (parts.length > 1 ? parts[1] : "") + " " + f.getEntryPoint() + " " + f.getName());
                Set<Function> path = new HashSet<>();
                path.add(f);
                walk(f, 1, path);
            }
        }
    }
}
