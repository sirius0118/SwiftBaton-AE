"""Locate sampled migration zeros without treating workload termination as downtime."""
import math

def select_zero_run(raw, checkpoint, cutover, gate=None):
    """raw: (wall, elapsed, cumulative count, rate, interval, completed).

    Retain all zero runs for diagnostics. Only closed runs overlapping the
    independently recorded cutover are eligible for the migration annotation.
    No samples or operation counts are invented. A missing match stays missing.
    """
    runs=[];active=[]
    for row in raw:
        if row[1]>=checkpoint and row[3]==0:
            active.append(row[1])
        elif active:
            runs.append(dict(samples=active,closed=row[5]>0));active=[]
    if active:runs.append(dict(samples=active,closed=False))
    if gate is not None:
        left,right=gate;left-=.05;right+=.05
    elif math.isfinite(cutover):
        left,right=cutover-2.,cutover+2.
    else:
        return [],runs
    choices=[r['samples'] for r in runs if r['closed'] and r['samples'][0]<=right and r['samples'][-1]>=left]
    return max(choices,key=lambda x:x[-1]-x[0]) if choices else [],runs
