/**
 * LightNVR Web Interface RecordingsView Component
 * Preact component for the recordings page
 */

import { h } from 'preact';
import { html } from '../../html-helper.js';
import { useState, useEffect, useRef } from 'preact/hooks';
import { showStatusMessage, showVideoModal, DeleteConfirmationModal } from './UI.js';
import { ContentLoader } from './LoadingIndicator.js';

// Import components
import { FiltersSidebar } from './recordings/FiltersSidebar.js';
import { ActiveFilters } from './recordings/ActiveFilters.js';
import { RecordingsTable } from './recordings/RecordingsTable.js';
import { PaginationControls } from './recordings/PaginationControls.js';

// Import utilities
import { formatUtils } from './recordings/formatUtils.js';
import { recordingsAPI } from './recordings/recordingsAPI.js';
import { urlUtils } from './recordings/urlUtils.js';
import { useQueryClient, invalidateQueries } from '../../query-client.js';

/**
 * RecordingsView component
 * @returns {JSX.Element} RecordingsView component
 */
export function RecordingsView() {
  const [recordings, setRecordings] = useState([]);
  const [streams, setStreams] = useState([]);
  const [filtersVisible, setFiltersVisible] = useState(true);
  const [sortField, setSortField] = useState('start_time');
  const [sortDirection, setSortDirection] = useState('desc');
  const [filters, setFilters] = useState({
    dateRange: 'last7days',
    startDate: '',
    startTime: '00:00',
    endDate: '',
    endTime: '23:59',
    streamId: 'all',
    recordingType: 'all'
  });
  const [pagination, setPagination] = useState({
    currentPage: 1,
    pageSize: 20,
    totalItems: 0,
    totalPages: 1,
    startItem: 0,
    endItem: 0
  });
  const [hasActiveFilters, setHasActiveFilters] = useState(false);
  const [activeFiltersDisplay, setActiveFiltersDisplay] = useState([]);
  const [selectedRecordings, setSelectedRecordings] = useState({});
  const [selectAll, setSelectAll] = useState(false);
  const [isDeleteModalOpen, setIsDeleteModalOpen] = useState(false);
  const [deleteMode, setDeleteMode] = useState('selected'); // 'selected' or 'all'
  const recordingsTableBodyRef = useRef(null);

  // Get query client for invalidating queries
  const queryClient = useQueryClient();

  // Fetch streams using preact-query
  const {
    data: streamsData,
    isLoading: isLoadingStreams,
    error: streamsError
  } = recordingsAPI.hooks.useStreams();

  // Update streams state when data is loaded
  useEffect(() => {
    if (streamsData && Array.isArray(streamsData)) {
      setStreams(streamsData);
    }
  }, [streamsData]);

  // Handle streams error
  useEffect(() => {
    if (streamsError) {
      console.error('Error loading streams for filter:', streamsError);
      showStatusMessage('Error loading streams: ' + streamsError.message);
    }
  }, [streamsError]);

  // Initialize component
  useEffect(() => {
    // Set default date range
    setDefaultDateRange();

    // Check for URL parameters
    const urlFilters = urlUtils.getFiltersFromUrl();

    if (urlFilters) {
      console.log('Found URL filters:', urlFilters);

      // Check specifically for detection parameter
      const urlParams = new URLSearchParams(window.location.search);
      if (urlParams.has('detection') && urlParams.get('detection') === '1') {
        // Ensure recordingType is set to 'detection'
        urlFilters.filters.recordingType = 'detection';
      }

      // Update state with URL filters
      setFilters(urlFilters.filters);
      setPagination(prev => ({
        ...prev,
        currentPage: urlFilters.page || 1,
        pageSize: urlFilters.limit || 20
      }));
      setSortField(urlFilters.sort || 'start_time');
      setSortDirection(urlFilters.order || 'desc');
    }

    // Handle responsive behavior
    handleResponsiveFilters();
    window.addEventListener('resize', handleResponsiveFilters);

    // Cleanup
    return () => {
      window.removeEventListener('resize', handleResponsiveFilters);
    };
  }, []);

  // Update active filters when filters change
  useEffect(() => {
    updateActiveFilters();
  }, [filters]);

  // Set default date range
  const setDefaultDateRange = () => {
    const now = new Date();
    const sevenDaysAgo = new Date(now);
    sevenDaysAgo.setDate(now.getDate() - 7);

    setFilters(prev => ({
      ...prev,
      endDate: now.toISOString().split('T')[0],
      startDate: sevenDaysAgo.toISOString().split('T')[0]
    }));
  };

  // Fetch recordings using preact-query
  const {
    data: recordingsData,
    isLoading: isLoadingRecordings,
    error: recordingsError,
    refetch: refetchRecordings
  } = recordingsAPI.hooks.useRecordings(filters, pagination, sortField, sortDirection);

  // Update recordings state when data is loaded
  useEffect(() => {
    if (recordingsData) {
      // Store recordings in the component state
      const recordingsArray = recordingsData.recordings || [];
      setRecordings(recordingsArray);
      setHasData(recordingsArray.length > 0);

      // Update pagination
      if (recordingsData.pagination) {
        updatePaginationFromResponse(recordingsData, pagination.currentPage);
      }
    }
  }, [recordingsData]);

  // Handle recordings error
  useEffect(() => {
    if (recordingsError) {
      console.error('Error loading recordings:', recordingsError);
      showStatusMessage('Error loading recordings: ' + recordingsError.message);
      setHasData(false);
    }
  }, [recordingsError]);

  // Load filters from URL
  const loadFiltersFromUrl = () => {
    const urlFilters = urlUtils.getFiltersFromUrl();
    if (urlFilters) {
      // Check specifically for detection parameter
      const urlParams = new URLSearchParams(window.location.search);
      if (urlParams.has('detection') && urlParams.get('detection') === '1') {
        // Ensure recordingType is set to 'detection'
        urlFilters.filters.recordingType = 'detection';
      }

      setFilters(urlFilters.filters);
      setPagination(prev => ({
        ...prev,
        currentPage: urlFilters.page || 1,
        pageSize: urlFilters.limit || 20
      }));
      setSortField(urlFilters.sort || 'start_time');
      setSortDirection(urlFilters.order || 'desc');

      // If detection parameter is present, ensure it's included in the URL when we update it
      if (urlParams.has('detection') && urlParams.get('detection') === '1') {
        setTimeout(() => {
          const currentUrl = new URL(window.location.href);
          if (!currentUrl.searchParams.has('detection')) {
            currentUrl.searchParams.set('detection', '1');
            window.history.replaceState({ path: currentUrl.href }, '', currentUrl.href);
          }
        }, 0);
      }

      return urlFilters; // Return the filters so we can use them directly
    }
    return null;
  };

  // Handle responsive filters
  const handleResponsiveFilters = () => {
    // On mobile, hide filters by default
    if (window.innerWidth < 768) {
      setFiltersVisible(false);
    } else {
      setFiltersVisible(true);
    }
  };

  // Toggle filters visibility
  const toggleFilters = () => {
    setFiltersVisible(!filtersVisible);
  };

  // State for data status
  const [hasData, setHasData] = useState(false);

  // Load recordings (now just updates pagination and URL)
  const loadRecordings = (page = pagination.currentPage, updateUrl = true) => {
    // Debug log to check filters
    console.log('Loading recordings with filters:', JSON.stringify(filters));

    // Create a pagination object with the specified page
    const paginationWithPage = {
      ...pagination,
      currentPage: page
    };

    // Update pagination state
    setPagination(paginationWithPage);

    // Update URL with filters if requested
    if (updateUrl) {
      urlUtils.updateUrlWithFilters(filters, paginationWithPage, sortField, sortDirection);
    }
  };

  // Update pagination from API response
  const updatePaginationFromResponse = (data, currentPage) => {
    // Use the provided page parameter instead of the state
    currentPage = currentPage || pagination.currentPage;

    if (data.pagination) {
      const pageSize = data.pagination.limit || 20;
      const totalItems = data.pagination.total || 0;
      const totalPages = data.pagination.pages || 1;

      // Calculate start and end items based on current page
      let startItem = 0;
      let endItem = 0;

      if (data.recordings.length > 0) {
        startItem = (currentPage - 1) * pageSize + 1;
        endItem = Math.min(startItem + data.recordings.length - 1, totalItems);
      }

      console.log('Pagination update:', {
        currentPage,
        pageSize,
        totalItems,
        totalPages,
        startItem,
        endItem,
        recordingsLength: data.recordings.length
      });

      setPagination(prev => ({
        ...prev,
        totalItems,
        totalPages,
        pageSize,
        startItem,
        endItem
      }));
    } else {
      // Fallback if pagination object is not provided
      const pageSize = pagination.pageSize;
      const totalItems = data.total || 0;
      const totalPages = Math.ceil(totalItems / pageSize) || 1;

      // Calculate start and end items based on current page
      let startItem = 0;
      let endItem = 0;

      if (data.recordings.length > 0) {
        startItem = (currentPage - 1) * pageSize + 1;
        endItem = Math.min(startItem + data.recordings.length - 1, totalItems);
      }

      console.log('Pagination update (fallback):', {
        currentPage,
        pageSize,
        totalItems,
        totalPages,
        startItem,
        endItem,
        recordingsLength: data.recordings.length
      });

      setPagination(prev => ({
        ...prev,
        totalItems,
        totalPages,
        startItem,
        endItem
      }));
    }
  };

  // Handle date range change
  const handleDateRangeChange = (e) => {
    const newDateRange = e.target.value;

    setFilters(prev => ({
      ...prev,
      dateRange: newDateRange
    }));

    if (newDateRange === 'custom') {
      // If custom is selected, make sure we have default dates
      if (!filters.startDate || !filters.endDate) {
        const now = new Date();
        const sevenDaysAgo = new Date(now);
        sevenDaysAgo.setDate(now.getDate() - 7);

        setFilters(prev => ({
          ...prev,
          endDate: now.toISOString().split('T')[0],
          startDate: sevenDaysAgo.toISOString().split('T')[0]
        }));
      }
    }
  };

  // Update active filters
  const updateActiveFilters = () => {
    const activeFilters = urlUtils.getActiveFiltersDisplay(filters);
    setHasActiveFilters(activeFilters.length > 0);
    setActiveFiltersDisplay(activeFilters);
  };

  // Apply filters
  const applyFilters = (resetToFirstPage = true) => {
    // Reset to first page when applying filters (unless specified otherwise)
    if (resetToFirstPage) {
      setPagination(prev => ({
        ...prev,
        currentPage: 1
      }));
    }

    // Update URL with filters
    urlUtils.updateUrlWithFilters(
      filters,
      resetToFirstPage ? {...pagination, currentPage: 1} : pagination,
      sortField,
      sortDirection
    );
  };

  // Reset filters
  const resetFilters = () => {
    // Create default filters
    const defaultFilters = {
      dateRange: 'last7days',
      startDate: '',
      startTime: '00:00',
      endDate: '',
      endTime: '23:59',
      streamId: 'all',
      recordingType: 'all'
    };

    // Get default date range
    const now = new Date();
    const sevenDaysAgo = new Date(now);
    sevenDaysAgo.setDate(now.getDate() - 7);

    defaultFilters.endDate = now.toISOString().split('T')[0];
    defaultFilters.startDate = sevenDaysAgo.toISOString().split('T')[0];

    // Reset filter state
    setFilters(defaultFilters);

    // Reset pagination to first page
    setPagination(prev => ({
      ...prev,
      currentPage: 1
    }));

    // Reset sort
    setSortField('start_time');
    setSortDirection('desc');

    // Clear all URL parameters by replacing the current URL with the base URL
    const baseUrl = window.location.pathname;
    window.history.pushState({ path: baseUrl }, '', baseUrl);
  };

  // Remove filter
  const removeFilter = (key) => {
    switch (key) {
      case 'dateRange':
        setFilters(prev => ({
          ...prev,
          dateRange: 'last7days'
        }));
        break;
      case 'streamId':
        setFilters(prev => ({
          ...prev,
          streamId: 'all'
        }));
        break;
      case 'recordingType':
        setFilters(prev => ({
          ...prev,
          recordingType: 'all'
        }));
        break;
    }

    applyFilters();
  };

  // Sort by field
  const sortBy = (field) => {
    if (sortField === field) {
      // Toggle direction if already sorting by this field
      setSortDirection(sortDirection === 'asc' ? 'desc' : 'asc');
    } else {
      // Default to descending for start_time, ascending for others
      setSortDirection(field === 'start_time' ? 'desc' : 'asc');
      setSortField(field);
    }

    // Reset to first page
    setPagination(prev => ({
      ...prev,
      currentPage: 1
    }));

    // Update URL with new sort parameters
    urlUtils.updateUrlWithFilters(
      filters,
      {...pagination, currentPage: 1},
      field,
      field === sortField ? (sortDirection === 'asc' ? 'desc' : 'asc') : (field === 'start_time' ? 'desc' : 'asc')
    );
  };

  // Go to page
  const goToPage = (page) => {
    if (page < 1 || page > pagination.totalPages) return;

    // Set the current page in pagination state
    setPagination(prev => ({
      ...prev,
      currentPage: page
    }));

    // Update URL with all filters and the new page
    urlUtils.updateUrlWithFilters(filters, {...pagination, currentPage: page}, sortField, sortDirection);
  };

  // Toggle selection of a recording
  const toggleRecordingSelection = (recordingId) => {
    setSelectedRecordings(prev => ({
      ...prev,
      [recordingId]: !prev[recordingId]
    }));
  };

  // Toggle select all recordings
  const toggleSelectAll = () => {
    const newSelectAll = !selectAll;
    setSelectAll(newSelectAll);

    const newSelectedRecordings = {};
    if (newSelectAll) {
      // Select all recordings
      recordings.forEach(recording => {
        newSelectedRecordings[recording.id] = true;
      });
    }
    // Always update selectedRecordings, even when deselecting all
    setSelectedRecordings(newSelectedRecordings);
  };

  // Get count of selected recordings
  const getSelectedCount = () => {
    return Object.values(selectedRecordings).filter(Boolean).length;
  };

  // Open delete confirmation modal
  const openDeleteModal = (mode) => {
    setDeleteMode(mode);
    setIsDeleteModalOpen(true);
  };

  // Close delete confirmation modal
  const closeDeleteModal = () => {
    setIsDeleteModalOpen(false);
  };

  // Delete selected recordings using preact-query mutation
  const { mutate: batchDeleteMutation } = recordingsAPI.hooks.useBatchDeleteRecordings();

  // Handle delete confirmation
  const handleDeleteConfirm = () => {
    closeDeleteModal();

    if (deleteMode === 'selected') {
      // Get selected IDs
      const selectedIds = Object.entries(selectedRecordings)
        .filter(([_, isSelected]) => isSelected)
        .map(([id, _]) => parseInt(id, 10));

      // Call the mutation with the selected IDs
      batchDeleteMutation({ ids: selectedIds });

      // Reset selection
      setSelectedRecordings({});
      setSelectAll(false);
    } else {
      // Create filter object for deleting all filtered recordings
      const filter = {};

      // Add date range filters
      if (filters.dateRange === 'custom') {
        filter.start = `${filters.startDate}T${filters.startTime}:00`;
        filter.end = `${filters.endDate}T${filters.endTime}:00`;
      } else {
        // Convert predefined range to actual dates
        const { start, end } = recordingsAPI.getDateRangeFromPreset(filters.dateRange);
        filter.start = start;
        filter.end = end;
      }

      // Add stream filter
      if (filters.streamId !== 'all') {
        filter.stream_name = filters.streamId;
      }

      // Add recording type filter
      if (filters.recordingType === 'detection') {
        filter.detection = 1;
      }

      // Call the mutation with the filter
      batchDeleteMutation({ filter });

      // Reset selection
      setSelectedRecordings({});
      setSelectAll(false);
    }
  };

  // Helper function to reload recordings with preserved parameters
  const reloadRecordingsWithPreservedParams = (sortField, sortDirection, page) => {
    // Set the sort parameters directly
    setSortField(sortField);
    setSortDirection(sortDirection);

    // Update pagination with the preserved page
    setPagination(prev => ({
      ...prev,
      currentPage: page
    }));

    // Wait for state to update
    setTimeout(() => {
      // Create a new pagination object with the updated page
      const updatedPagination = {
        ...pagination,
        currentPage: page
      };

      // Update URL with all filters and the preserved parameters
      urlUtils.updateUrlWithFilters(filters, updatedPagination, sortField, sortDirection);

      // Load recordings from API
      recordingsAPI.loadRecordings(filters, updatedPagination, sortField, sortDirection)
        .then(data => {
          console.log('Recordings data received:', data);

          // Store recordings in the component state
          setRecordings(data.recordings || []);

          // Update pagination without changing the current page
          updatePaginationFromResponse(data, page);
        })
        .catch(error => {
          console.error('Error loading recordings:', error);
          showStatusMessage('Error loading recordings: ' + error.message);
        });
    }, 0);
  };

  // Delete recording using preact-query mutation
  const { mutate: deleteRecordingMutation } = recordingsAPI.hooks.useDeleteRecording();

  // Delete a single recording
  const deleteRecording = (recording) => {
    if (!confirm(`Are you sure you want to delete this recording from ${recording.stream}?`)) {
      return;
    }

    // Call the mutation with the recording ID
    deleteRecordingMutation(recording.id);
  };

  // Play recording
  const playRecording = (recording) => {
    recordingsAPI.playRecording(recording, showVideoModal);
  };

  // Download recording
  const downloadRecording = (recording) => {
    recordingsAPI.downloadRecording(recording);
  };

  return html`
    <section id="recordings-page" class="page">
      <div class="page-header flex justify-between items-center mb-4 p-4 bg-white dark:bg-gray-800 rounded-lg shadow">
        <div class="flex items-center">
          <h2 class="text-xl font-bold">Recordings</h2>
          <div class="ml-4 flex">
            <a href="recordings.html" class="px-3 py-1 bg-blue-500 text-white rounded-l-md">Table View</a>
            <a href="timeline.html" class="px-3 py-1 bg-gray-300 text-gray-700 dark:bg-gray-700 dark:text-gray-300 hover:bg-gray-400 dark:hover:bg-gray-600 rounded-r-md">Timeline View</a>
          </div>
        </div>
        <button id="toggle-filters-btn"
                class="p-2 rounded-full bg-gray-200 hover:bg-gray-300 dark:bg-gray-700 dark:hover:bg-gray-600 focus:outline-none"
                title="Toggle Filters"
                onClick=${toggleFilters}>
          <svg class="w-5 h-5" fill="currentColor" viewBox="0 0 20 20" xmlns="http://www.w3.org/2000/svg">
            <path fill-rule="evenodd" d="M3 5a1 1 0 011-1h12a1 1 0 110 2H4a1 1 0 01-1-1zM3 10a1 1 0 011-1h12a1 1 0 110 2H4a1 1 0 01-1-1zM3 15a1 1 0 011-1h12a1 1 0 110 2H4a1 1 0 01-1-1z" clip-rule="evenodd"></path>
          </svg>
        </button>
      </div>

      <div class="recordings-layout flex flex-col md:flex-row gap-4 w-full">
        <!-- Sidebar for filters -->
        <${FiltersSidebar}
          filters=${filters}
          setFilters=${setFilters}
          pagination=${pagination}
          setPagination=${setPagination}
          streams=${streams}
          filtersVisible=${filtersVisible}
          applyFilters=${applyFilters}
          resetFilters=${resetFilters}
          handleDateRangeChange=${handleDateRangeChange}
          setDefaultDateRange=${setDefaultDateRange}
        />

        <!-- Main content area -->
        <div class="recordings-content flex-1">
          <${ActiveFilters}
            activeFiltersDisplay=${activeFiltersDisplay}
            removeFilter=${removeFilter}
            hasActiveFilters=${hasActiveFilters}
          />

          <${ContentLoader}
            isLoading=${isLoadingRecordings}
            hasData=${hasData}
            loadingMessage="Loading recordings..."
            emptyMessage="No recordings found matching your criteria"
          >
            <${RecordingsTable}
              recordings=${recordings}
              sortField=${sortField}
              sortDirection=${sortDirection}
              sortBy=${sortBy}
              selectedRecordings=${selectedRecordings}
              toggleRecordingSelection=${toggleRecordingSelection}
              selectAll=${selectAll}
              toggleSelectAll=${toggleSelectAll}
              getSelectedCount=${getSelectedCount}
              openDeleteModal=${openDeleteModal}
              playRecording=${playRecording}
              downloadRecording=${downloadRecording}
              deleteRecording=${deleteRecording}
              recordingsTableBodyRef=${recordingsTableBodyRef}
              pagination=${pagination}
            />

            <${PaginationControls}
              pagination=${pagination}
              goToPage=${goToPage}
            />
          <//>
        </div>
      </div>

      <${DeleteConfirmationModal}
        isOpen=${isDeleteModalOpen}
        onClose=${closeDeleteModal}
        onConfirm=${handleDeleteConfirm}
        mode=${deleteMode}
        count=${getSelectedCount()}
      />
    </section>
  `;
}

/**
 * Load RecordingsView component
 */
export function loadRecordingsView() {
  const mainContent = document.getElementById('main-content');
  if (!mainContent) return;

  // Render the RecordingsView component to the container
  import('preact').then(({ render }) => {
    import('../../query-client.js').then(({ QueryClientProvider, queryClient }) => {
      render(
        html`<${QueryClientProvider} client=${queryClient}><${RecordingsView} /></${QueryClientProvider}>`,
        mainContent
      );
    });
  });
}
