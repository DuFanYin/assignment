import databento as db

path = "src/data/CLX5_mbo.dbn"

store = db.DBNStore.from_file(path)

print("Schema:", store.schema)
print("Symbols:", store.symbols)
print("Metadata:", store.metadata)

# 转为 pandas DataFrame
df = store.to_df()
print(df.head(10))  # 只打印前10行