import os
import argparse

def rename_glb_files(directory, dry_run=False):
    # Ensure the directory exists
    if not os.path.isdir(directory):
        print(f"Error: {directory} is not a valid directory.")
        return

    print(f"--- {'DRY RUN' if dry_run else 'EXECUTING'} ---")
    
    for filename in os.listdir(directory):
        # Only process .glb files
        if not filename.endswith('.glb'):
            continue
            
        name_part, extension = os.path.splitext(filename)
        
        # Split by underscore
        parts = name_part.split('_')
        
        # Only rename if it's not already just X.glb
        if len(parts) > 1:
            suffix_x = parts[-1]
            remaining_prefix = "_".join(parts[:-1])
            new_name = f"{suffix_x}_{remaining_prefix}{extension}"
            
            old_path = os.path.join(directory, filename)
            new_path = os.path.join(directory, new_name)
            
            if dry_run:
                print(f"[WOULD RENAME] {filename} -> {new_name}")
            else:
                print(f"[RENAMED] {filename} -> {new_name}")
                os.rename(old_path, new_path)
        else:
            print(f"[SKIPPING] {filename} (no underscore found)")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Extract suffix X from A_B_X.glb and move it to the front.")
    parser.add_argument("path", help="Path to the directory containing .glb files")
    parser.add_argument("--run", action="store_true", help="Actually perform the rename (default is dry run)")

    args = parser.parse_args()

    # If --run is NOT provided, dry_run is True
    rename_glb_files(args.path, dry_run=not args.run)
