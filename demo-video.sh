#!/bin/bash

# Create a video with a black background and a sine wave audio track
function create_video() {
    echo "Creating video..."
    ffmpeg -f lavfi -i color=c=black:s=480x360:r=30:d=60 \
    -f lavfi -i sine=frequency=261.63:duration=1:sample_rate=44100 \
    -f lavfi -i sine=frequency=293.66:duration=1:sample_rate=44100 \
    -f lavfi -i sine=frequency=329.63:duration=1:sample_rate=44100 \
    -f lavfi -i sine=frequency=349.23:duration=1:sample_rate=44100 \
    -f lavfi -i sine=frequency=392.00:duration=1:sample_rate=44100 \
    -f lavfi -i sine=frequency=440.00:duration=1:sample_rate=44100 \
    -f lavfi -i sine=frequency=493.88:duration=1:sample_rate=44100 \
    -filter_complex "\
    [0:v]drawtext=timecode='00\:00\:00\:00':rate=30:fontcolor=white:fontsize=24:x=(w-tw)/2:y=(h-th)/2[v]; \
    [1:a][2:a][3:a][4:a][5:a][6:a][7:a]concat=n=7:v=0:a=1[seq]; [seq]aloop=7:size=308700[a]" \
    -map "[v]" -map "[a]" -c:v libx264 -c:a aac -t 60 "$1.mp4"
}

# check for ffmpeg
if ! command -v ffmpeg &> /dev/null
then
    echo "ffmpeg could not be found"
    exit 1
fi

# get project root directory
PROJECT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/" && pwd)

echo "PROJECT_DIR: $PROJECT_DIR"

# default output name
OUTPUT_NAME="$PROJECT_DIR/demo"
echo "OUTPUT_NAME: $OUTPUT_NAME.mp4"

# create a demo video
create_video $OUTPUT_NAME
