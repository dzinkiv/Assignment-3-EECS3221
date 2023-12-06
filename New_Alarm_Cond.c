// Include necessary headers
#include <pthread.h> // For threading functions and types

#include <time.h> // For time-related functions

#include "errors.h" // Assumed to be a custom header for error handling (not standard C)

#include <semaphore.h> // For semaphore functions and types

#include <stdio.h> // For standard input/output functions

#include <stdlib.h> // For standard library functions

// Define the structure for an alarm
typedef struct alarm {
  int id; // Unique identifier for the alarm
  int group_id; // Group ID to categorize alarms
  int seconds; // Duration in seconds after which the alarm should act
  time_t time; // Timestamp when the alarm was set
  char message[128]; // A message associated with the alarm
  pthread_t display_thread_id; // ID of the thread responsible for displaying this alarm
  struct alarm * next; // Pointer to the next alarm in a linked list
}
alarm_t;

// Define the structure for a change request
typedef struct change_request {
  int alarm_id; // ID of the alarm to be changed
  int new_group_id; // New group ID for the alarm
  int new_seconds; // New duration in seconds for the alarm
  time_t new_time; // New timestamp when the alarm is set
  char new_message[128]; // New message for the alarm
  struct change_request * next; // Pointer to the next change request in a linked list
}
change_request_t;

// Define the structure for a node in the alarm queue
typedef struct alarm_queue_node {
  alarm_t * alarm; // Pointer to the alarm
  int reassigned; // Indicates if the alarm has been reassigned to a different group
  int message_changed; // Indicates if the alarm's message has been changed
  struct alarm_queue_node * next; // Pointer to the next node in the queue
}
alarm_queue_node_t;

// Define the structure for a thread node
typedef struct thread_node {
  pthread_t thread_id; // ID of the thread
  int group_id; // Group ID that this thread is responsible for
  int alarm_count; // Count of alarms this thread is managing
  alarm_queue_node_t * alarm_queue; // Queue of alarms that this thread is responsible for
  pthread_mutex_t queue_mutex; // Mutex for synchronizing access to the alarm queue
  pthread_cond_t queue_cond; // Condition variable for signaling changes in the alarm queue
  struct thread_node * next; // Pointer to the next thread node in the list
}
thread_node_t;

// Global variables
alarm_t * alarm_list = NULL; // Head of the linked list of alarms
change_request_t * change_request_list = NULL; // Head of the linked list of change requests
thread_node_t * display_alarm_thread_list = NULL; // Head of the linked list of display alarm threads

// Mutexes and condition variables for synchronization
pthread_mutex_t alarm_list_mutex = PTHREAD_MUTEX_INITIALIZER; // Mutex for synchronizing access to the alarm list
pthread_cond_t alarm_cond = PTHREAD_COND_INITIALIZER; // Condition variable for the alarm list

pthread_mutex_t change_request_list_mutex = PTHREAD_MUTEX_INITIALIZER; // Mutex for synchronizing access to the change request list
pthread_cond_t change_request_cond = PTHREAD_COND_INITIALIZER; // Condition variable for the change request list

pthread_mutex_t display_alarm_thread_list_mutex = PTHREAD_MUTEX_INITIALIZER; // Mutex for synchronizing access to the display alarm thread list

// Function prototype declaration for the display alarm thread function
void * display_alarm_thread_function(void * arg);

// Function to add a thread node to the linked list of display alarm threads
thread_node_t * add_thread_node(thread_node_t ** head, pthread_t tid, int group_id) {

  // Allocate memory for a new thread node
  thread_node_t * new_node = (thread_node_t * ) malloc(sizeof(thread_node_t));
  if (new_node == NULL) {
    perror("Failed to allocate memory for thread node");
    exit(1); // Exit if memory allocation fails
  }
  // Initialize the new node with provided thread ID and group ID
  new_node -> thread_id = tid;
  new_node -> group_id = group_id;
  new_node -> alarm_count = 0; // Initialize alarm count as zero
  new_node -> alarm_queue = NULL; // Initialize the alarm queue as empty
  pthread_mutex_init( & new_node -> queue_mutex, NULL); // Initialize the mutex for the alarm queue
  pthread_cond_init( & new_node -> queue_cond, NULL); // Initialize the condition variable for the alarm queue
  new_node -> next = * head; // Link the new node to the current head of the list
  * head = new_node; // Update the head of the list to point to the new node
  
  return new_node; // Return the newly created node
}

// Function to signal a specific display thread
void signal_display_thread(pthread_t thread_id, int alarm_id, int reassigned, int message_changed) {
  // Iterate through the list of display alarm threads
  for (thread_node_t * current = display_alarm_thread_list; current != NULL; current = current -> next) {
    // Check if the current thread's ID matches the provided thread ID
    if (pthread_equal(current -> thread_id, thread_id)) {
      pthread_mutex_lock( & current -> queue_mutex); // Lock the mutex before accessing the queue
      // Iterate through the alarm queue of the thread
      for (alarm_queue_node_t * queue_node = current -> alarm_queue; queue_node != NULL; queue_node = queue_node -> next) {
        // Check if the current alarm in the queue matches the provided alarm ID
        if (queue_node -> alarm -> id == alarm_id) {
          queue_node -> reassigned = reassigned; // Set the reassignment flag
          queue_node -> message_changed = message_changed; // Set the message changed flag
          break; // Break the loop once the alarm is found and updated
        }
      }
      pthread_mutex_unlock( & current -> queue_mutex); // Unlock the mutex
      pthread_cond_signal( & current -> queue_cond); // Signal the condition variable to notify the thread of changes
      break; // Break the loop once the target thread is found and signaled
    }
  }
}

// Function to find the thread ID managing an alarm with a given ID
pthread_t find_thread_id_by_alarm_id(int alarm_id) {
  // Iterate through the list of display alarm threads
  for (thread_node_t * current = display_alarm_thread_list; current != NULL; current = current -> next) {
    // Iterate through each alarm in the thread's alarm queue
    for (alarm_queue_node_t * node = current -> alarm_queue; node != NULL; node = node -> next) {
      // Check if the current alarm matches the given alarm ID
      if (node -> alarm -> id == alarm_id) {
        return current -> thread_id; // Return the thread ID if found
      }
    }
  }
  return (pthread_t) 0; // Return an invalid thread ID if not found
}

// Function to find or create a display thread for a specific group ID
pthread_t find_or_create_thread_for_group(int group_id) {
  pthread_t thread_id = 0; // Initialize thread ID as zero (invalid thread ID)

  // Lock the mutex to access the global display alarm thread list safely
  pthread_mutex_lock( & display_alarm_thread_list_mutex);

  // Iterate through existing display alarm threads
  for (thread_node_t * current = display_alarm_thread_list; current != NULL; current = current -> next) {
    // Check if a thread already exists for the specified group ID
    if (current -> group_id == group_id) {
      thread_id = current -> thread_id; // Assign the found thread ID
      break; // Exit loop as the thread has been found
    }
  }

  // If no thread exists for the given group ID, create a new thread
  if (thread_id == 0) {
    // Allocate memory for the group argument to be passed to the new thread
    int * group_arg = malloc(sizeof(int));
    if (group_arg == NULL) {
      perror("Failed to allocate memory for group argument");
      exit(1); // Exit if memory allocation fails
    }
    * group_arg = group_id; // Set the group ID in the allocated memory

    // Create a new display alarm thread
    if (pthread_create( & thread_id, NULL, display_alarm_thread_function, group_arg) != 0) {
      perror("Failed to create a new display alarm thread");
      exit(1); // Exit if thread creation fails
    }

    // Add the new thread to the global display alarm thread list
    add_thread_node( & display_alarm_thread_list, thread_id, group_id);
  }

  // Unlock the mutex after processing
  pthread_mutex_unlock( & display_alarm_thread_list_mutex);

  return thread_id; // Return the found or newly created thread ID
}

// Function for the alarm monitor thread
void * alarm_monitor_thread_function(void * arg) {
  // Retrieve the thread ID of the alarm monitor thread
  pthread_t monitor_thread_id = pthread_self();

  // Infinite loop to continuously monitor alarms
  while (1) {

    int status;
    struct timespec cond_time;
    int expired = 0;

    time_t now;
    time( & now); // Get the current time

    // Convert the current time to struct tm
    struct tm * timeinfo = localtime( & now);

    // Lock the mutex to access the alarm list
    pthread_mutex_lock( & alarm_list_mutex);
    //pthread_cond_wait(&alarm_cond, &alarm_list_mutex);

    if (alarm_list != NULL) {
      //DEBUG
      printf("Next alarm set for: %s", ctime(&(alarm_list->time)));

      // Check if the next alarm time is in the future
      if (alarm_list -> time > now) {
        // Set up the time until which to wait
        cond_time.tv_sec = alarm_list -> time;
        cond_time.tv_nsec = 0;

        while (!expired) {
          status = pthread_cond_timedwait( & alarm_cond, & alarm_list_mutex, & cond_time);
          if (status == ETIMEDOUT) {
            //DEBUG
            printf("Alarm expired.\n");
            expired = 1;
            break;
          }
          if (status != 0)
            err_abort(status, "Cond timedwait");
        }
      } else {
        //DEBUG
        printf("Alarm already expired.\n");
        expired = 1;
      }

      if (expired) {
        // Handle the expired alarm
        alarm_t * expired_alarm = alarm_list;
        alarm_list = alarm_list -> next;

        // Signal the display thread to stop displaying this alarm
        signal_display_thread(expired_alarm -> display_thread_id, expired_alarm -> id, -1, 0);

        // Create a string to hold the formatted date and time
        char formatted_current_time[80];
        strftime(formatted_current_time, sizeof(formatted_current_time), "%H:%M:%S", timeinfo);

        // Print a message indicating the removal of the alarm
        printf("Alarm Monitor Thread %lu Has Removed Alarm(%d) at %s: Group(%d) %s\n",
          (unsigned long) monitor_thread_id, expired_alarm -> id, formatted_current_time,
          expired_alarm -> group_id, expired_alarm -> message);

        free(expired_alarm);
      }
    }
    else {
      //DEBUG
      printf("No alarms set.\n");
    }

    // Unlock the mutex for the alarm list
    pthread_mutex_unlock( & alarm_list_mutex);

    // Wait for a new change request to be made
    pthread_mutex_lock( & change_request_list_mutex);
    //pthread_cond_wait(&change_request_cond, &change_request_list_mutex);

    // Process each change request in the list
    change_request_t * prev_request = NULL;
    change_request_t * current_request = change_request_list;
    while (current_request != NULL) {
      // Lock the mutex to access the alarm list
      pthread_mutex_lock( & alarm_list_mutex);

      // Search for the alarm corresponding to the change request
      alarm_t * alarm = alarm_list;
      while (alarm != NULL) {
        if (alarm -> id == current_request -> alarm_id) {
          // Store the original group ID for comparison
          int old_group_id = alarm -> group_id;

          // Check if the message of the alarm has changed
          int message_changed = strncmp(alarm -> message, current_request -> new_message, sizeof(alarm -> message)) != 0;

          // Update the alarm with the details from the change request
          alarm -> group_id = current_request -> new_group_id;
          alarm -> time = current_request -> new_time;
          strncpy(alarm -> message, current_request -> new_message, sizeof(alarm -> message));

          // If the group ID of the alarm has changed, handle reassignment
          if (old_group_id != alarm -> group_id) {
            // Signal the old display thread to stop printing this alarm
            signal_display_thread(alarm -> display_thread_id, alarm -> id, -1, 0);

            // Find or create a new thread for the updated group ID and update the alarm's display_thread_id
            pthread_t new_thread_id = find_or_create_thread_for_group(alarm -> group_id);
            alarm -> display_thread_id = new_thread_id;

            // Signal the new display thread to start displaying this alarm
            signal_display_thread(new_thread_id, alarm -> id, 1, 0);
          }

          // If the message of the alarm has changed, signal the display thread
          if (message_changed) {
            signal_display_thread(alarm -> display_thread_id, alarm -> id, 0, message_changed);
          }

          // Print a message indicating that the alarm has been changed
          printf("Alarm Monitor Thread %lu Has Changed Alarm(%d) at %s: Group(%d) %s\n",
            (unsigned long) monitor_thread_id, alarm -> id, alarm -> time, alarm -> group_id, alarm -> message);

          break; // Exit the loop as the relevant alarm has been updated
        }
        alarm = alarm -> next; // Move to the next alarm
      }

      // If the alarm corresponding to the change request is not found, print an invalid change request message
      if (alarm == NULL) {
        printf("Invalid Change Alarm Request(%d) at %s: Group(%d) %s\n",
          current_request -> alarm_id, current_request -> new_time, current_request -> new_group_id, current_request -> new_message);
      }

      // Unlock the mutex for the alarm list
      pthread_mutex_unlock( & alarm_list_mutex);

      // Remove the processed change request from the list
      change_request_t * temp = current_request;
      if (prev_request == NULL) {
        change_request_list = current_request -> next;
      } else {
        prev_request -> next = current_request -> next;
      }
      current_request = current_request -> next;

      // Free the memory allocated for the processed change request
      free(temp);
    }

    // Unlock the mutex for the change request list
    pthread_mutex_unlock( & change_request_list_mutex);

    // Sleep for a short duration before checking again
    sleep(1); // Prevents continuous spinning and excessive CPU usage
  }
  return NULL;
}

// Function for the display alarm thread
void * display_alarm_thread_function(void * arg) {
  // Extract thread information from the argument
  thread_node_t * thread_info = (thread_node_t * ) arg;
  // Retrieve the thread ID of the display alarm thread
  pthread_t display_thread_id = pthread_self();
  time_t now;

  // Continuous loop to handle alarm display
  while (1) {
    // Lock the mutex to safely access the alarm queue of this thread
    pthread_mutex_lock( & thread_info -> queue_mutex);
    // Wait for a signal to process alarms (either new or updated)
    pthread_cond_wait( & thread_info -> queue_cond, & thread_info -> queue_mutex);

    // Iterate through the alarm queue of this thread
    alarm_queue_node_t * queue_node = thread_info -> alarm_queue;
    alarm_queue_node_t * prev_node = NULL;
    while (queue_node != NULL) {
      // Get the current alarm from the queue node
      alarm_t * alarm = queue_node -> alarm;

      // Handle different scenarios based on alarm status flags
      if (queue_node -> reassigned == 1) {
        // Handle the case where this thread has taken over a reassigned alarm
        time( & now);
        char formatted_time[80];
        strftime(formatted_time, 80, "%H:%M:%S", localtime( & now));
        printf("Display Thread %lu Has Taken Over Printing Message of Alarm(%d) at %s: Changed Group(%d) %s\n",
          (unsigned long) display_thread_id, alarm -> id, formatted_time, alarm -> group_id, alarm -> message);
        queue_node -> reassigned = 0; // Reset the flag
      } else if (queue_node -> reassigned == -1) {
        // Handle the case where this thread stops printing an alarm
        time( & now);
        char formatted_time[80];
        strftime(formatted_time, 80, "%H:%M:%S", localtime( & now));
        printf("Display Thread %lu Has Stopped Printing Message of Alarm(%d) at %s: Changed Group(%d) %s\n",
          (unsigned long) display_thread_id, alarm -> id, formatted_time, alarm -> group_id, alarm -> message);

        // Remove the alarm from this thread's queue
        if (prev_node == NULL) {
          thread_info -> alarm_queue = queue_node -> next;
          free(queue_node);
          queue_node = thread_info -> alarm_queue;
        } else {
          prev_node -> next = queue_node -> next;
          free(queue_node);
          queue_node = prev_node -> next;
        }
        continue; // Skip to the next iteration
      } else if (queue_node -> message_changed) {
        // Handle the case where the alarm message has been changed
        time( & now);
        char formatted_time[80];
        strftime(formatted_time, 80, "%Y-%m-%d %H:%M:%S", localtime( & now));
        printf("Display Thread %lu Starts to Print Changed Message Alarm(%d) at %s: Group(%d) %s\n",
          (unsigned long) display_thread_id, alarm -> id, formatted_time, alarm -> group_id, alarm -> message);
        queue_node -> message_changed = 0; // Reset the flag
      } else {
        // Regular printing of the alarm information
        time( & now);
        char formatted_time[80];
        strftime(formatted_time, 80, "%Y-%m-%d %H:%M:%S", localtime( & now));
        printf("Alarm (%d) Printed by Alarm Display Thread %lu at %s: Group(%d) %s\n",
          alarm -> id, (unsigned long) display_thread_id, formatted_time, alarm -> group_id, alarm -> message);
      }

      // Move to the next queue node
      prev_node = queue_node;
      queue_node = queue_node -> next;
    }

    // Check if there are no more alarms to display for this thread
    if (thread_info -> alarm_queue == NULL) {
      // Print an exit message and break the loop to terminate the thread
      time( & now);
      char formatted_time[80];
      strftime(formatted_time, 80, "%Y-%m-%d %H:%M:%S", localtime( & now));
      printf("No More Alarms in Group(%d): Display Thread %lu exiting at %s\n",
        thread_info -> group_id, (unsigned long) display_thread_id, formatted_time);
      pthread_mutex_unlock( & thread_info -> queue_mutex);
      break; // Exit the while loop and end the thread
    }

    // Unlock the mutex and sleep for a specified duration
    pthread_mutex_unlock( & thread_info -> queue_mutex);
    sleep(5); // Sleep for 5 seconds as specified in the requirements
  }

  return NULL;
}

// Function to insert a new alarm into the global alarm list in sorted order
void alarm_insert(alarm_t * alarm) {
  int status;
  alarm_t ** last, * next;

  // Initialize pointers to start at the head of the list
  last = & alarm_list;
  next = * last;
  while (next != NULL) {
    // If the next alarm ID is greater or equal, insert the new alarm here
    if (next -> id >= alarm -> id) {
      alarm -> next = next;
      * last = alarm;
      break;
    }

    // Move to the next node in the list
    last = & next -> next;
    next = next -> next;

  }

  // If at the end of the list, append the new alarm
  if (next == NULL) {
    * last = alarm;
    alarm -> next = NULL;
  }

  // Signal the condition variable to indicate a new alarm has been inserted
  status = pthread_cond_signal( & alarm_cond);
  if (status != 0) {
    err_abort(status, "Signal cond");
  }

}

// Function to insert a change request into the change request list in sorted order
void insert_change_request(change_request_t ** head, change_request_t * new_request) {
  int status;

  // Initialize pointers to start at the head of the list
  change_request_t ** last = head;
  change_request_t * next = * head;

  // Find the correct position to insert (sorted by alarm_id)
  while (next != NULL) {
    if (next -> alarm_id >= new_request -> alarm_id) {
      // Insert the new request before the next one
      new_request -> next = next;
      * last = new_request;
      break;
    }
    // Move to the next node in the list
    last = & next -> next;
    next = next -> next;
  }

  // If at the end of the list, append the new request
  if (next == NULL) {
    * last = new_request;
    new_request -> next = NULL;
  }

  // Signal the condition variable to indicate a new change request has been inserted
  status = pthread_cond_signal( & change_request_cond);
  if (status != 0) {
    err_abort(status, "Signal cond");
  }
}

int main(int argc, char * argv[]) {
  char line[128];

  // Store the thread ID of the main thread for future reference
  pthread_t main_thread_id = pthread_self();

  // Create the alarm monitor thread
  pthread_t alarm_monitor_thread;
  pthread_create( & alarm_monitor_thread, NULL, alarm_monitor_thread_function, NULL);

  // Main loop for processing user input
  while (1) {
    printf("Alarm> ");
    if (fgets(line, sizeof(line), stdin) == NULL) exit(0); // Exit if input fails
    if (strlen(line) <= 1) continue; // Ignore empty lines

    int alarm_id, group_id, seconds;
    char message[128];

    // Process the Start_Alarm command
    if (sscanf(line, "Start_Alarm(%d): Group(%d) %d %128[^\n]", & alarm_id, & group_id, & seconds, message) == 4) {
      // Create and initialize a new alarm structure
      alarm_t * new_alarm = (alarm_t * ) malloc(sizeof(alarm_t));
      if (new_alarm == NULL) {
        errno_abort("Allocate alarm"); // Handle allocation error
      }
      new_alarm->id = alarm_id;
      new_alarm->group_id = group_id;
      new_alarm->seconds = seconds;
      new_alarm->time = time(NULL) + seconds; // Set the alarm to expire 'seconds' from now
      strncpy(new_alarm->message, message, sizeof(new_alarm->message));


      // Insert the new alarm into the global alarm list
      pthread_mutex_lock( & alarm_list_mutex);
      alarm_insert(new_alarm);
      pthread_mutex_unlock( & alarm_list_mutex);

      // Check if a new display alarm thread needs to be created
      int * group_arg = malloc(sizeof(int));
      * group_arg = group_id;
      pthread_t new_thread;
      int thread_created = 0;

      pthread_mutex_lock( & display_alarm_thread_list_mutex);
      for (thread_node_t * current = display_alarm_thread_list; current != NULL; current = current -> next) {
        if (current -> group_id == group_id && current -> alarm_count < 2) {
          // Add the alarm to an existing thread's queue
          pthread_mutex_lock( & current -> queue_mutex);
          alarm_queue_node_t * new_queue_node = (alarm_queue_node_t * ) malloc(sizeof(alarm_queue_node_t));
          new_queue_node -> alarm = new_alarm;
          new_queue_node -> next = current -> alarm_queue;
          current -> alarm_queue = new_queue_node;
          pthread_mutex_unlock( & current -> queue_mutex);

          current -> alarm_count++;
          thread_created = 1;
          new_alarm -> display_thread_id = current -> thread_id; // Update thread ID in alarm

          printf("Main Thread %lu Assigned to Display Alarm(%d) at %s: Group(%d) %s\n",
            (unsigned long) main_thread_id, new_alarm -> id, new_alarm -> time, new_alarm -> group_id, new_alarm -> message);
          break;
        }
      }
      pthread_mutex_unlock( & display_alarm_thread_list_mutex);

      if (!thread_created) {
        // Create a new display alarm thread if necessary
        pthread_create( & new_thread, NULL, display_alarm_thread_function, group_arg);
        pthread_mutex_lock( & display_alarm_thread_list_mutex);
        add_thread_node( & display_alarm_thread_list, new_thread, group_id);
        pthread_mutex_unlock( & display_alarm_thread_list_mutex);

        new_alarm -> display_thread_id = new_thread; // Update thread ID in alarm

        char time_str[30]; // Ensure this buffer is large enough
        strftime(time_str, sizeof(time_str), "%H:%M:%S", localtime( & (new_alarm -> time)));

        printf("Main Thread Created New Display Alarm Thread %lu For Alarm(%d) at %s: Group(%d) %s\n\n",
          (unsigned long) new_thread, new_alarm -> id, time_str, new_alarm -> group_id, new_alarm -> message);

      }
      char time_str[30]; // Buffer to hold the formatted time string
      strftime(time_str, sizeof(time_str), "%H:%M:%S", localtime( & (new_alarm -> time)));

      printf("Alarm(%d) Inserted by Main Thread %ld Into Alarm List at %s: Group(%d) %s\n\n",
        new_alarm -> id, (unsigned long) main_thread_id, time_str, new_alarm -> group_id, new_alarm -> message);
    } else if (sscanf(line, "Change_Alarm(%d): Group(%d) %d %128[^\n]", & alarm_id, & group_id, & seconds, message) == 4) {
      // Process the Change_Alarm command
      change_request_t * new_request = (change_request_t * ) malloc(sizeof(change_request_t));
      if (new_request == NULL) {
        errno_abort("Allocate alarm"); // Handle allocation error
      }
      new_request->alarm_id = alarm_id;
      new_request->new_group_id = group_id;
      new_request->new_seconds = seconds; // Duration from now until the alarm should expire
      // Set the new expiry time as the current time plus the specified duration
      new_request->new_time = time(NULL) + seconds; 
      strncpy(new_request->new_message, message, sizeof(new_request->new_message));

      pthread_mutex_lock( & change_request_list_mutex);
      insert_change_request( & change_request_list, new_request);
      pthread_mutex_unlock( & change_request_list_mutex);

      printf("Change Alarm Request(%d) Inserted by Main Thread %ld Into Alarm List at %s: Group(%d) %s\n",
        new_request -> alarm_id, (unsigned long) main_thread_id, new_request -> new_time, new_request -> new_group_id, new_request -> new_message);
    } else {
      fprintf(stderr, "Invalid command format or bad command.\n");
    }
  }

  return 0;
}