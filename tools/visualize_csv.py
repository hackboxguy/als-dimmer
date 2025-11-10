#!/usr/bin/env python3
"""
Visualize ALS-Dimmer CSV logs

Examples:
    python3 visualize_csv.py --input=/tmp/test_data.csv
    python3 visualize_csv.py --input=/tmp/test.csv --output=/tmp/plot.jpg
    python3 visualize_csv.py --input=/tmp/test.csv --output=/tmp/plot.pdf --no-show
"""

import sys
import argparse
import pandas as pd
import matplotlib.pyplot as plt
from pathlib import Path

def parse_arguments():
    parser = argparse.ArgumentParser(
        description='Visualize ALS-Dimmer control loop data from CSV logs',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s --input=/tmp/data.csv
  %(prog)s --input=/tmp/data.csv --output=/tmp/plot.jpg
  %(prog)s --input=/tmp/data.csv --output=/tmp/plot.pdf --no-show
  %(prog)s -i /tmp/data.csv -o /tmp/plot.png --dpi=300
        """
    )

    parser.add_argument('--input', '-i',
                        required=True,
                        help='Input CSV file path (required)')

    parser.add_argument('--output', '-o',
                        help='Output image file path (default: <input>.png)')

    parser.add_argument('--dpi',
                        type=int,
                        default=150,
                        help='Output image DPI/resolution (default: 150)')

    parser.add_argument('--no-show',
                        action='store_true',
                        help="Don't display plot interactively, just save to file")

    parser.add_argument('--format',
                        choices=['png', 'jpg', 'jpeg', 'pdf', 'svg'],
                        help='Output format (auto-detected from --output extension if not specified)')

    args = parser.parse_args()

    # Validate input file exists
    if not Path(args.input).exists():
        parser.error(f"Input file not found: {args.input}")

    # Set default output path if not specified
    if args.output is None:
        args.output = str(Path(args.input).with_suffix('.png'))

    # Auto-detect format from output extension if not specified
    if args.format is None:
        ext = Path(args.output).suffix.lstrip('.').lower()
        if ext in ['jpg', 'jpeg', 'png', 'pdf', 'svg']:
            args.format = 'jpeg' if ext == 'jpg' else ext
        else:
            # Default to PNG if extension is unrecognized
            args.format = 'png'
            if not args.output.endswith('.png'):
                args.output += '.png'

    return args

# Parse command-line arguments
args = parse_arguments()

# Load CSV data
print(f"Loading data from: {args.input}")
try:
    df = pd.read_csv(args.input)
except Exception as e:
    print(f"Error loading CSV: {e}")
    sys.exit(1)

if len(df) == 0:
    print("Error: CSV file is empty")
    sys.exit(1)

print(f"Loaded {len(df)} data points")

# Create figure with subplots
fig, axes = plt.subplots(4, 1, figsize=(12, 10), sharex=True)

# Plot 1: Lux and Brightness
ax1 = axes[0]
ax1.plot(df['timestamp'], df['lux'], 'g-', label='Lux', linewidth=2)
ax1.set_ylabel('Lux', color='g')
ax1.tick_params(axis='y', labelcolor='g')
ax1.grid(True, alpha=0.3)

ax1_twin = ax1.twinx()
ax1_twin.plot(df['timestamp'], df['target_brightness'], 'b--', label='Target', linewidth=1.5, alpha=0.7)
ax1_twin.plot(df['timestamp'], df['current_brightness'], 'r-', label='Actual', linewidth=2)
ax1_twin.set_ylabel('Brightness (%)', color='r')
ax1_twin.tick_params(axis='y', labelcolor='r')
ax1_twin.legend(loc='upper right')
ax1.set_title('Control Loop Response: Lux → Brightness')

# Plot 2: Error and Step Size
ax2 = axes[1]
ax2.plot(df['timestamp'], df['error'], 'b-', label='Error', linewidth=2)
ax2.axhline(y=0, color='k', linestyle='--', alpha=0.3)
ax2.set_ylabel('Error (%)', color='b')
ax2.tick_params(axis='y', labelcolor='b')
ax2.grid(True, alpha=0.3)

ax2_twin = ax2.twinx()
ax2_twin.plot(df['timestamp'], df['step_size'], 'r-', label='Step Size', linewidth=2)
ax2_twin.set_ylabel('Step Size (%)', color='r')
ax2_twin.tick_params(axis='y', labelcolor='r')
ax2.set_title('Error vs Step Size')

# Plot 3: Step Categories
ax3 = axes[2]
step_colors = {
    'large_up': 'red',
    'large_down': 'orange',
    'medium_up': 'yellow',
    'medium_down': 'lightyellow',
    'small_up': 'lightgreen',
    'small_down': 'lightblue',
    'none': 'gray',
    'manual': 'purple'
}
for category in df['step_category'].unique():
    mask = df['step_category'] == category
    ax3.scatter(df[mask]['timestamp'], [category] * mask.sum(),
                label=category, color=step_colors.get(category, 'black'), s=50)
ax3.set_ylabel('Step Category')
ax3.set_title('Adaptive Step Sizing')
ax3.legend(loc='upper right', ncol=4)
ax3.grid(True, alpha=0.3)

# Plot 4: Zone Transitions
ax4 = axes[3]
zones = df['zone'].unique()
zone_colors = {'night': 'darkblue', 'indoor': 'yellow', 'outdoor': 'orange',
               'simple': 'gray', 'manual': 'purple'}
for zone in zones:
    mask = df['zone'] == zone
    ax4.scatter(df[mask]['timestamp'], [zone] * mask.sum(),
                label=zone, color=zone_colors.get(zone, 'black'), s=50)

# Mark zone changes with vertical lines
zone_changes = df[df['zone_changed'] == 1]
for _, row in zone_changes.iterrows():
    ax4.axvline(x=row['timestamp'], color='red', linestyle='--', alpha=0.5, linewidth=0.5)

ax4.set_xlabel('Time (seconds)')
ax4.set_ylabel('Zone')
ax4.set_title('Zone Transitions')
ax4.legend(loc='upper right')
ax4.grid(True, alpha=0.3)

plt.tight_layout()

# Save figure
print(f"Saving visualization to: {args.output}")
try:
    plt.savefig(args.output, format=args.format, dpi=args.dpi, bbox_inches='tight')
    print(f"✓ Saved successfully (format={args.format}, dpi={args.dpi})")
except Exception as e:
    print(f"Error saving plot: {e}")
    sys.exit(1)

# Display interactively if requested
if not args.no_show:
    print("Displaying plot (close window to continue)...")
    plt.show()
else:
    print("Skipping interactive display (--no-show)")

# Print summary statistics
print("\n" + "="*60)
print("SUMMARY STATISTICS")
print("="*60)
print(f"Total iterations: {len(df)}")
print(f"Duration: {df['timestamp'].max():.1f} seconds")
print(f"Update interval: ~{(df['timestamp'].max() / len(df)):.3f} seconds")
print(f"Lux range: {df['lux'].min():.1f} - {df['lux'].max():.1f}")
print(f"Brightness range: {df['current_brightness'].min():.0f}% - {df['current_brightness'].max():.0f}%")
print(f"Zones visited: {', '.join(df['zone'].unique())}")
print(f"Zone changes: {df['zone_changed'].sum()}")
print(f"\nStep category distribution:")
print(df['step_category'].value_counts())
print(f"\nAverage error: {df['error'].abs().mean():.2f}%")
print(f"Max error: {df['error'].abs().max():.0f}%")
