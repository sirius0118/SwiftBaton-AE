package site.ycsb;

import java.io.BufferedWriter;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;

/** Per-worker completion gaps; consumes the DBWrapper's existing nanoTime sample.
 * No clocks, allocation, locks or file writes in record(). The intersection of
 * all worker gaps gives the exact global no-success intervals above 5 ms.
 * Missing workers or overflow invalidate that measurement, never mean zero.
 */
final class SuccessGapJournal {
  static final long MIN_GAP_NS = 5000000L;
  private final long[] starts = new long[32768];
  private final long[] ends = new long[32768];
  private final Path output;
  private final long anchorBefore = System.nanoTime();
  private final long anchorWallMs = System.currentTimeMillis();
  private final long anchorAfter = System.nanoTime();
  private long first, last, successes, dropped;
  private int count;

  SuccessGapJournal(String directory) {
    output = Paths.get(directory, "worker-" + Thread.currentThread().getId() + ".csv");
  }

  void record(long end) {
    if (successes != 0 && end - last >= MIN_GAP_NS) {
      if (count == starts.length) {
        dropped++;
      } else {
        starts[count] = last;
        ends[count++] = end;
      }
    }
    if (successes == 0) { first = end; }
    last = end;
    successes++;
  }

  void close() throws IOException {
    Files.createDirectories(output.getParent());
    try (BufferedWriter w = Files.newBufferedWriter(output, StandardCharsets.UTF_8)) {
      w.write("# first=" + first + " last=" + last + " successes=" + successes
          + " dropped=" + dropped + " count=" + count + " threshold_ns=" + MIN_GAP_NS
          + " anchor_before=" + anchorBefore + " anchor_after=" + anchorAfter
          + " anchor_wall_ms=" + anchorWallMs + "\nstart_ns,end_ns\n");
      for (int i = 0; i < count; i++) {
        w.write(starts[i] + "," + ends[i] + "\n");
      }
    }
  }
}
