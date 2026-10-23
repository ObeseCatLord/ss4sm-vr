// Headless: decompile the functions containing each address listed in a file.
// args: <addresses file (hex per line, optional "name" after whitespace)> <output file>
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import java.io.*;
import java.nio.file.*;
import java.util.*;

public class DecompileList extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        List<String> lines = Files.readAllLines(Paths.get(args[0]));
        DecompInterface d = new DecompInterface();
        DecompileOptions o = new DecompileOptions();
        d.setOptions(o);
        d.openProgram(currentProgram);
        try (PrintWriter w = new PrintWriter(new FileWriter(args[1]))) {
            Set<Address> done = new HashSet<>();
            for (String line : lines) {
                line = line.trim();
                if (line.isEmpty() || line.startsWith("#")) continue;
                String[] parts = line.split("\\s+", 2);
                Address a = toAddr(Long.parseUnsignedLong(parts[0].replace("0x", ""), 16));
                Function f = getFunctionContaining(a);
                if (f == null) {
                    createFunction(a, null);
                    f = getFunctionAt(a);
                }
                if (f == null) { w.println("// no function at " + parts[0]); continue; }
                if (!done.add(f.getEntryPoint())) continue;
                DecompileResults r = d.decompileFunction(f, 120, monitor);
                w.println("// ===== " + (parts.length > 1 ? parts[1] : "") + " @ " + f.getEntryPoint() + " (" + f.getName() + ")");
                w.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// decompile failed: " + r.getErrorMessage());
            }
        }
    }
}
