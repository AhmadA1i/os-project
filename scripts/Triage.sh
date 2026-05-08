#!/bin/bash
# Triage.sh - Patient intake and triage assessment
# Usage: ./Triage.sh <name> <age> <severity 1-10>

TRIAGE_FIFO="/tmp/triage_fifo"

# ---- Validation Function ----
validate_fun(){
    local name=$1
    local age=$2
    local sov=$3

    if [ -z "$name" ]; then
        echo "Error: Name can't be empty"
        return 1
    fi

    if ! [[ "$age" =~ ^[0-9]+$ ]]; then
        echo "Error: Age must be a positive number"
        return 1
    fi

    if (( age <= 0 || age > 150 )); then
        echo "Error: Age must be between 1 and 150"
        return 1
    fi

    if ! [[ "$sov" =~ ^[0-9]+$ ]]; then
        echo "Error: Severity must be a number"
        return 1
    fi

    if (( sov < 1 || sov > 10 )); then
        echo "Error: Severity must be between 1 and 10"
        return 1
    fi

    return 0
}

# ---- Priority from Severity ----
comp_priority(){
    local sov=$1
    case $sov in
        9|10) echo 1 ;;
        7|8)  echo 2 ;;
        5|6)  echo 3 ;;
        3|4)  echo 4 ;;
        1|2)  echo 5 ;;
        *)    echo 0 ;;
    esac
}

# ---- Bed Type from Priority ----
Bed_type(){
    local pri=$1
    case $pri in
        1|2) echo "ICU" ;;
        3)   echo "ISOLATION" ;;
        4|5) echo "GENERAL" ;;
        *)   echo "UNKNOWN" ;;
    esac
}

# ---- Care Units from Bed Type ----
get_care_unit(){
    local type=$1
    case $type in
        "ICU")       echo 3 ;;
        "ISOLATION") echo 2 ;;
        "GENERAL")   echo 1 ;;
        *)           echo 0 ;;
    esac
}

# ---- Main Program ----

if [ $# -ne 3 ]; then
    echo "Usage: ./Triage.sh <name> <age> <severity 1-10>"
    exit 1
fi

Name="$1"
Age="$2"
Sov="$3"

if ! validate_fun "$Name" "$Age" "$Sov"; then
    exit 1
fi

priority=$(comp_priority "$Sov")
Bed=$(Bed_type "$priority")
Unit=$(get_care_unit "$Bed")
Pat_Id=$(( ($(date +%s) % 10000) + (RANDOM % 1000) ))
Arrival_time=$(date '+%Y-%m-%d %H:%M:%S')

echo "============================================"
echo "  TRIAGE ASSESSMENT COMPLETE"
echo "============================================"
echo "  Patient ID     : $Pat_Id"
echo "  Name           : $Name"
echo "  Age            : $Age"
echo "  Severity       : $Sov/10"
echo "  Priority       : $priority"
echo "  Bed Type       : $Bed"
echo "  Care Units     : $Unit"
echo "  Arrival Time   : $Arrival_time"
echo "============================================"

RECORD="$Pat_Id|$Name|$Age|$Sov|$priority|$Unit|$Bed|$Arrival_time"

# Send to FIFO if hospital is running, otherwise print to screen
if [[ -p "$TRIAGE_FIFO" ]]; then
    echo "$RECORD" > "$TRIAGE_FIFO"
    echo "Patient record sent to admissions via FIFO."
else
    echo "Admissions FIFO not found. Record printed to stdout only."
    echo "Raw Record: $RECORD"
fi

exit 0
