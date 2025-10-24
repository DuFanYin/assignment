#!/usr/bin/env python3
import databento as db
import pandas as pd

def export_dbn_to_csv():
    """Export DBN file to CSV for C++ processing"""
    
    print("Loading DBN file...")
    path = "src/data/CLX5_mbo.dbn"
    store = db.DBNStore.from_file(path)
    df = store.to_df()
    
    print(f"Loaded {len(df)} rows")
    print(f"Columns: {list(df.columns)}")
    
    # Export to CSV (without index to avoid ts_recv column issues)
    csv_path = "src/data/CLX5_mbo.csv"
    df.to_csv(csv_path, index=False)
    
    print(f"Exported to: {csv_path}")
    print("First 5 rows:")
    print(df.head())
    
    return csv_path

if __name__ == "__main__":
    export_dbn_to_csv()