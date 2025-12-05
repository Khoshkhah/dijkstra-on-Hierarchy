from pyspark.sql import SparkSession
from pyspark.sql.functions import *
import random

spark = SparkSession.builder.appName("CHTestData").getOrCreate()

N_EDGES = 10_000      # change to your real size
N_SHORTCUTS = 50_000  # ~5× edges is realistic

random.seed(42)

data = []
for i in range(N_SHORTCUTS):
    from_edge = random.randint(0, N_EDGES-1)
    to_edge   = random.randint(0, N_EDGES-1)
    while to_edge == from_edge:
        to_edge = random.randint(0, N_EDGES-1)

    data.append((
        from_edge,
        to_edge,
        random.randint(1, 10000),           # cost
        random.randint(1000000, 9999999),   # via_edge (just for unpacking)
        random.randint(0, 63),              # via_cell (partition)
        random.choice([-1, 0, 1])           # inside flag
    ))

df = spark.createDataFrame(data, [
    "incoming_edge", "outgoing_edge", "cost",
    "via_edge", "via_cell", "inside"
])

df.repartition(64).write \
  .mode("overwrite") \
  .option("compression", "zstd") \
  .parquet("data/ch_shortcuts.parquet")

print("Generated data/ch_shortcuts.parquet")
spark.stop()